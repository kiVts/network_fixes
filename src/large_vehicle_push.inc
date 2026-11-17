constexpr DWORD kLargePushFreshMs=150;
constexpr double kLargePushMaxN=1500;
using VehicleSamples_t=int(__cdecl*)(SwmlVehiclePushSample*,int,uint32_t*);
using AddForce_t=void(__fastcall*)(uint8_t*,uint8_t*,const float*,const float*,uint8_t);
VehicleSamples_t g_vehicle_samples=nullptr;
AddForce_t g_add_force=nullptr;
DWORD g_vehicle_samples_checked=0;
bool g_large_push_on=true;
#pragma pack(push,1)
struct WireVehicleForce {uint32_t id;uint16_t body;float point[3],force[3];};
#pragma pack(pop)
std::vector<WireVehicleForce> g_force_out;
uint32_t g_force_out_seq=0,g_force_sent_seq=0;
struct PeerForces {uint32_t seq=0;DWORD at=0;std::vector<WireVehicleForce> forces;};
std::map<int,PeerForces> g_peer_forces;
uint64_t g_force_samples=0,g_force_packets=0,g_force_applied=0,g_force_rejected=0;
double g_force_max_n=0,g_force_sum_n=0,g_force_max_lever=0,g_force_min_kg=0,g_force_max_accel=0;
// Second form (vehicle_push2.h): clients send what their character does, hosts that announce it (kPushCap2) turn it
// into the forces the host's own character would apply. Older hosts keep getting the first form.
constexpr double kPush2Step=1.0/60,kPush2MaxPushN=20000,kPush2MaxLandN=60000;
#pragma pack(push,1)
struct WireVehiclePush2 {uint32_t id;uint16_t body;float point[3],dir[3];float approach,support,land,kg;};
#pragma pack(pop)
static_assert(sizeof(WireVehiclePush2)==46);
VehicleSamples2_t g_vehicle_samples2=nullptr;
std::vector<WireVehiclePush2> g_force_out2;
uint32_t g_force_out2_seq=0,g_force_sent2_seq=0;
struct PeerPush2 {uint32_t seq=0,landed_seq=0;DWORD at=0;std::vector<WireVehiclePush2> pushes;};
std::map<int,PeerPush2> g_peer_push2;
uint64_t g_push2_samples=0,g_push2_packets=0,g_push2_applied=0,g_push2_landings=0,g_push2_rejected=0;
double g_push2_max_push=0,g_push2_max_support=0,g_push2_max_land=0;

// The vehicle id and body index of a host-replayed body, unless one of this client's own units holds it.
bool replayed_body_index(const RingView& vehicles,const void* body,uint32_t& id,uint16_t& index)
{
    for(uint32_t i=0;i<vehicles.count;i++) {
        auto v=vehicles.at(i);RingView br;if(!v || !vehicle_bodies(v,br) || br.count>4096)continue;
        uint32_t vid=*reinterpret_cast<const uint32_t*>(v+kVehicleId);
        bool owned=false;for(const auto& u:g_units)owned=owned || (u.vehicle && u.id==vid);
        if(owned)continue;
        for(uint32_t k=0;k<br.count;k++) {
            auto so=vehicle_body_so(br,k);auto b=so?*reinterpret_cast<uint8_t**>(so+kSoBody):nullptr;
            if(b==body){id=vid;index=uint16_t(k);return true;}
        }
    }
    return false;
}

void collect_vehicle_push2(const RingView& vehicles)
{
    if(!g_vehicle_samples2)return;
    VehiclePushSample2 samples[16];uint32_t seq=0;int n=g_vehicle_samples2(samples,16,&seq);
    if(!seq || seq==g_force_out2_seq || n<0 || n>16)return;
    g_force_out2.clear();g_force_out2_seq=seq;
    for(int j=0;j<n;j++) {
        WireVehiclePush2 w{};
        if(!replayed_body_index(vehicles,samples[j].body,w.id,w.body))continue;
        memcpy(w.point,samples[j].point,sizeof w.point);memcpy(w.dir,samples[j].dir,sizeof w.dir);
        w.approach=samples[j].approach;w.support=samples[j].support;w.land=samples[j].land;w.kg=samples[j].kg;
        g_force_out2.push_back(w);++g_push2_samples;
    }
}

void collect_vehicle_forces(const RingView& vehicles)
{
    if(!g_large_push_on)return;
    DWORD now=tick_now();
    if(!g_vehicle_samples && now-g_vehicle_samples_checked>=1000) {
        g_vehicle_samples_checked=now;auto phys=GetModuleHandleW(L"network_fixes.dll"); // touch_census.cpp
        if(phys)g_vehicle_samples=reinterpret_cast<VehicleSamples_t>(GetProcAddress(phys,"swml_vehicle_push_samples"));
        if(phys)g_vehicle_samples2=reinterpret_cast<VehicleSamples2_t>(GetProcAddress(phys,"swml_vehicle_push_samples2"));
    }
    collect_vehicle_push2(vehicles);
    if(!g_vehicle_samples)return;
    SwmlVehiclePushSample samples[16];uint32_t seq=0;int n=g_vehicle_samples(samples,16,&seq);
    if(!seq || seq==g_force_out_seq || n<0 || n>16)return;
    g_force_out.clear();g_force_out_seq=seq;
    for(int j=0;j<n;j++) {
        bool found=false;
        for(uint32_t i=0;i<vehicles.count && !found;i++) {
            auto v=vehicles.at(i);RingView br;if(!v || !vehicle_bodies(v,br) || br.count>4096)continue;
            uint32_t id=*reinterpret_cast<const uint32_t*>(v+kVehicleId);
            bool owned=false;for(const auto& u:g_units)owned=owned || (u.vehicle && u.id==id);
            if(owned)continue;
            for(uint32_t k=0;k<br.count;k++) {
                auto so=vehicle_body_so(br,k);auto body=so?*reinterpret_cast<uint8_t**>(so+kSoBody):nullptr;
                if(body!=samples[j].body)continue;
                WireVehicleForce f{};f.id=id;f.body=uint16_t(k);
                memcpy(f.point,samples[j].point,sizeof f.point);memcpy(f.force,samples[j].force,sizeof f.force);
                g_force_out.push_back(f);++g_force_samples;found=true;
                g_force_max_n=std::max(g_force_max_n,std::sqrt(double(f.force[0])*f.force[0]+double(f.force[1])*f.force[1]+double(f.force[2])*f.force[2]));
                break;
            }
        }
    }
}

void apply_vehicle_forces(uint8_t* scene,const RingView& vehicles,const double remote[][3],int nremote,DWORD now)
{
    if(!g_large_push_on || !g_add_force)return;
    for(auto it=g_peer_forces.begin();it!=g_peer_forces.end();) {
        if(now-it->second.at>kLargePushFreshMs){it=g_peer_forces.erase(it);continue;}
        double total=0;for(const auto& f:it->second.forces)total+=std::sqrt(double(f.force[0])*f.force[0]+double(f.force[1])*f.force[1]+double(f.force[2])*f.force[2]);
        double scale=total>kLargePushMaxN?kLargePushMaxN/total:1;
        for(const auto& f:it->second.forces) {
            auto v=find_vehicle(vehicles,f.id);RingView br;
            if(!v || !vehicle_bodies(v,br) || f.body>=br.count){++g_force_rejected;continue;}
            bool owned=false;for(const auto& [key,c]:g_claims)owned=owned || (c.s.id==f.id && c.s.kind>=kVehiclePart);
            if(owned)continue;
            auto so=vehicle_body_so(br,f.body);auto body=so?*reinterpret_cast<uint8_t**>(so+kSoBody):nullptr;
            if(!body || (*reinterpret_cast<const int*>(body+kBodyFlags)&(3|kFlagDriven)) ||
               !(*reinterpret_cast<const double*>(body+kBodyInvMass)>0)){++g_force_rejected;continue;}
            double force[3]{},offset[3]{},point[3];
            for(int a=0;a<3;a++)for(int k=0;k<3;k++) {
                double r=*reinterpret_cast<const double*>(body+kBodyBasis+a*0x20+k*8);
                force[a]+=r*f.force[k]*scale;offset[a]+=r*f.point[k];
            }
            force[1]=0;
            for(int a=0;a<3;a++)point[a]=offset[a]+*reinterpret_cast<const double*>(body+kBodyOrigin+8*a);
            bool contact_near=false;
            for(int j=0;j<nremote && !contact_near;j++) {
                double d[3]={point[0]-remote[j][0],point[1]-remote[j][1],point[2]-remote[j][2]};contact_near=length3(d)<3;
            }
            Box bounds;bool inside=body_box(body,bounds);
            for(int a=0;a<3 && inside;a++)inside=point[a]>=bounds.mn[a]-.5 && point[a]<=bounds.mx[a]+.5;
            if(!contact_near || !inside){++g_force_rejected;continue;}
            g_force_max_lever=std::max(g_force_max_lever,std::fabs(offset[1]));offset[1]=0;
            float F[3],P[3];for(int a=0;a<3;a++){F[a]=float(force[a]);P[a]=float(offset[a]);}
            g_add_force(scene,so,F,P,1);++g_force_applied;
            double n=length3(force),kg=1/ *reinterpret_cast<const double*>(body+kBodyInvMass);
            g_force_max_n=std::max(g_force_max_n,n);g_force_sum_n+=n;
            if(!(g_force_min_kg>0) || kg<g_force_min_kg)g_force_min_kg=kg;
            g_force_max_accel=std::max(g_force_max_accel,n/kg);
        }
        ++it;
    }
}

// Host: a client's character, as the forces this PC's own character would have put on the body (one physics step
// each). Pushing: the impulse of an inelastic contact between the character (its mass, walking at its approach speed)
// and the body, until the contact point moves away at that speed. Standing: the character's weight. Landing: the same
// contact impulse downward, once per landing. Applied at the centre of mass's height (a sideways force at the
// character's feet rolled cars onto their suspension); the horizontal lever stays, so a vertical force still tilts the
// vehicle where the character stands.
void apply_vehicle_push2(uint8_t* scene,const RingView& vehicles,const double remote[][3],int nremote,DWORD now)
{
    if(!g_large_push_on || !g_add_force)return;
    for(auto it=g_peer_push2.begin();it!=g_peer_push2.end();) {
        if(now-it->second.at>kLargePushFreshMs){it=g_peer_push2.erase(it);continue;}
        bool land_now=it->second.seq!=it->second.landed_seq;it->second.landed_seq=it->second.seq;
        for(const auto& f:it->second.pushes) {
            auto v=find_vehicle(vehicles,f.id);RingView br;
            if(!v || !vehicle_bodies(v,br) || f.body>=br.count){++g_push2_rejected;continue;}
            bool owned=false;for(const auto& [key,c]:g_claims)owned=owned || (c.s.id==f.id && c.s.kind>=kVehiclePart);
            if(owned)continue;
            auto so=vehicle_body_so(br,f.body);auto body=so?*reinterpret_cast<uint8_t**>(so+kSoBody):nullptr;
            double im=body?*reinterpret_cast<const double*>(body+kBodyInvMass):0;
            if(!body || (*reinterpret_cast<const int*>(body+kBodyFlags)&(3|kFlagDriven)) || !(im>0)){++g_push2_rejected;continue;}
            double dir[3]{},offset[3]{},point[3];
            for(int a=0;a<3;a++)for(int k=0;k<3;k++) {
                double r=*reinterpret_cast<const double*>(body+kBodyBasis+a*0x20+k*8);
                dir[a]+=r*f.dir[k];offset[a]+=r*f.point[k];
            }
            for(int a=0;a<3;a++)point[a]=offset[a]+*reinterpret_cast<const double*>(body+kBodyOrigin+8*a);
            bool contact_near=false;
            for(int j=0;j<nremote && !contact_near;j++) {
                double d[3]={point[0]-remote[j][0],point[1]-remote[j][1],point[2]-remote[j][2]};contact_near=length3(d)<3;
            }
            Box bounds;bool inside=body_box(body,bounds);
            for(int a=0;a<3 && inside;a++)inside=point[a]>=bounds.mn[a]-.5 && point[a]<=bounds.mx[a]+.5;
            if(!contact_near || !inside){++g_push2_rejected;continue;}
            // the contact point's velocity: v + w x r
            double lin[3],ang[3],u[3];
            memcpy(lin,body+kBodyLinVel,24);memcpy(ang,body+kBodyAngVel,24);
            u[0]=lin[0]+ang[1]*offset[2]-ang[2]*offset[1];
            u[1]=lin[1]+ang[2]*offset[0]-ang[0]*offset[2];
            u[2]=lin[2]+ang[0]*offset[1]-ang[1]*offset[0];
            double kg=std::clamp(double(f.kg),10.0,300.0),M=1/im,mu=kg*M/(kg+M);
            double F[3]{};bool any=false;
            double dl=std::hypot(dir[0],dir[2]);
            if(dl>.5 && std::isfinite(f.approach) && f.approach>0) {
                double n0=dir[0]/dl,n2=dir[2]/dl,need=std::min(double(f.approach),12.0)-(u[0]*n0+u[2]*n2);
                if(need>0) {
                    double push=std::min(mu*need/kPush2Step,kPush2MaxPushN);
                    F[0]+=push*n0;F[2]+=push*n2;any=true;g_push2_max_push=std::max(g_push2_max_push,push);
                }
            }
            if(std::isfinite(f.support) && f.support>0) {
                double w=std::min(double(f.support),kg*9.81*1.5);F[1]-=w;any=true;g_push2_max_support=std::max(g_push2_max_support,w);
            }
            if(land_now && std::isfinite(f.land) && f.land>0) {
                double need=std::min(double(f.land),30.0)-(-u[1]);
                if(need>0){double hit=std::min(mu*need/kPush2Step,kPush2MaxLandN);F[1]-=hit;any=true;++g_push2_landings;g_push2_max_land=std::max(g_push2_max_land,hit);}
            }
            if(!any)continue;
            float Ff[3],P[3];for(int a=0;a<3;a++){Ff[a]=float(F[a]);P[a]=float(offset[a]);}
            P[1]=0;
            g_add_force(scene,so,Ff,P,1);++g_push2_applied;
        }
        ++it;
    }
}
