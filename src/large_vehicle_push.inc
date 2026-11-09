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
double g_force_max_n=0;

void collect_vehicle_forces(const RingView& vehicles)
{
    if(!g_large_push_on)return;
    DWORD now=tick_now();
    if(!g_vehicle_samples && now-g_vehicle_samples_checked>=1000) {
        g_vehicle_samples_checked=now;auto phys=GetModuleHandleW(L"network_fixes.dll"); // touch_census.cpp
        if(phys)g_vehicle_samples=reinterpret_cast<VehicleSamples_t>(GetProcAddress(phys,"swml_vehicle_push_samples"));
    }
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
                g_force_out.push_back(f);++g_force_samples;found=true;break;
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
            float F[3],P[3];for(int a=0;a<3;a++){F[a]=float(force[a]);P[a]=float(offset[a]);}
            g_add_force(scene,so,F,P,1);++g_force_applied;g_force_max_n=std::max(g_force_max_n,length3(force));
        }
        ++it;
    }
}
