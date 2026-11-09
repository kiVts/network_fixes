// Native movable boxes (including pallets) are omitted by vanilla's cylinder-only replay.
// Publish host states for nearby boxes; unowned clients follow them through native queued events.
constexpr DWORD kHostBoxEveryMs = 50, kHostBoxFreshMs = 400;
constexpr double kHostBoxRadius = 40;
constexpr size_t kMaxHostBoxes = 128;
std::vector<WireState> g_host_box_out;
uint32_t g_host_box_seq = 0;
size_t g_host_box_cursor = 0;
DWORD g_host_box_at = 0;
struct HostBoxState { WireState s; uint32_t seq; DWORD at; };
std::map<uint64_t,HostBoxState> g_host_boxes;
uint64_t g_box_packets = 0, g_box_applied = 0, g_box_owned_skipped = 0;
double g_box_max_error = 0;

bool native_box_entry(const uint8_t* entry)
{
    auto handle = entry ? *reinterpret_cast<const uint8_t* const*>(entry+kEntryHandle) : nullptr;
    auto movable = handle ? *reinterpret_cast<const uint8_t* const*>(handle+kKinds[0].slot) : nullptr;
    return movable && *reinterpret_cast<const uint32_t*>(movable+kKind0Type) != kKind0TypeValue;
}

// Packet orientation is Bullet's body->world quaternion; game event transforms are column-major 4x4.
void host_box_transform(const WireState& s, double age, double out[16])
{
    age = std::clamp(age,0.0,kMaxAgeS);
    double x=s.q[0],y=s.q[1],z=s.q[2],w=s.q[3];
    double speed=std::sqrt(double(s.w[0])*s.w[0]+double(s.w[1])*s.w[1]+double(s.w[2])*s.w[2]);
    if (speed>1e-9 && age>0) {
        double half=.5*speed*age,f=std::sin(half)/speed,c=std::cos(half);
        double a=s.w[0]*f,b=s.w[1]*f,d=s.w[2]*f;
        double nx=c*x+a*w+b*z-d*y,ny=c*y-a*z+b*w+d*x;
        double nz=c*z+a*y-b*x+d*w,nw=c*w-a*x-b*y-d*z;
        x=nx;y=ny;z=nz;w=nw;
    }
    double norm=std::sqrt(x*x+y*y+z*z+w*w);
    if(norm<1e-9){x=y=z=0;w=norm=1;}
    x/=norm;y/=norm;z/=norm;w/=norm;
    out[0]=1-2*(y*y+z*z);out[1]=2*(x*y+w*z);out[2]=2*(x*z-w*y);out[3]=0;
    out[4]=2*(x*y-w*z);out[5]=1-2*(x*x+z*z);out[6]=2*(y*z+w*x);out[7]=0;
    out[8]=2*(x*z+w*y);out[9]=2*(y*z-w*x);out[10]=1-2*(x*x+y*y);out[11]=0;
    for(int i=0;i<3;i++)out[12+i]=s.p[i]+s.v[i]*age;
    out[15]=1;
}

void collect_host_boxes(const RingView& objects, const double remote[][3], int nremote, DWORD now)
{
    if (g_host_box_at && now-g_host_box_at < kHostBoxEveryMs) return;
    g_host_box_at=now;g_host_box_out.clear();g_host_box_cursor=0;++g_host_box_seq;
    if (!nremote) return;
    for(uint32_t i=0;i<objects.count && g_host_box_out.size()<kMaxHostBoxes;i++) {
        auto entry=objects.at(i);
        if(!native_box_entry(entry) || held_movable(entry))continue;
        auto so=scene_object_of(entry,0);
        auto body=so?*reinterpret_cast<uint8_t**>(so+kSoBody):nullptr;
        Box bounds;
        if(!body || (*reinterpret_cast<const int*>(body+kBodyFlags)&3) ||
           !body_box(body,bounds) || !box_ok(bounds,kMaxExtent))continue;
        auto p=reinterpret_cast<const double*>(body+kBodyOrigin);
        bool nearby=false;
        for(int j=0;j<nremote && !nearby;j++) {
            double d[3]={p[0]-remote[j][0],p[1]-remote[j][1],p[2]-remote[j][2]};
            nearby=length3(d)<kHostBoxRadius;
        }
        if(!nearby)continue;
        WireState s{};s.id=id_of(entry);s.kind=0;body_state(body,s);g_host_box_out.push_back(s);
    }
}

void follow_host_boxes(uint8_t* scene,const RingView& objects,DWORD now,bool host_ok)
{
    for(auto it=g_host_boxes.begin();it!=g_host_boxes.end();) {
        const HostBoxState& h=it->second;
        if(!host_ok || now-h.at>kHostBoxFreshMs){it=g_host_boxes.erase(it);continue;}
        auto entry=find_entry(objects,h.s.id);
        auto so=native_box_entry(entry)?scene_object_of(entry,0):nullptr;
        auto body=so?*reinterpret_cast<uint8_t**>(so+kSoBody):nullptr;
        if(held_movable(entry)){it=g_host_boxes.erase(it);continue;}
        if(!body || (*reinterpret_cast<const int*>(body+kBodyFlags)&3)){++it;continue;}
        if(is_owned_so(so)){++g_box_owned_skipped;++it;continue;}
        double d[3];for(int k=0;k<3;k++)d[k]=h.s.p[k]-*reinterpret_cast<const double*>(body+kBodyOrigin+8*k);
        g_box_max_error=std::max(g_box_max_error,length3(d));
        alignas(16) double transform[16];host_box_transform(h.s,(now-h.at)/1000.0,transform);
        g_orig_transform(scene,so,transform);
        g_set_lin(scene,so,h.s.v);g_set_ang(scene,so,h.s.w);
        ++g_box_applied;++it;
    }
}
