// Train control-loop recorder. All native reads happen at the existing simulation
// boundary or immediately around an existing component call; no new game hook.
#include "logic_trace.h"
#include "logic_trace_bridge.h"
#include "game.h"
#include "log.h"
#include "resolve.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
constexpr uint32_t kComponents=128,kPorts=64,kBodies=128,kLanes=16,kQueue=4096,kTotalPorts=1024;
enum Kind : uint32_t { SlotRecord=1,BodyRecord=2,BatchRecord=3,TickRecord=4,InventoryRecord=5,ComponentRecord=6,CorrectionRecord=7,MessageRecord=8,TargetRecord=9 };
struct Identity { uint32_t vehicle,body; int voxel[3]; uint32_t component_type; };
struct Record {
    Identity id{};
    uint32_t kind=0,tick=0,stage=0,role=0,unit=0,order=0,port=0,port_type=0,count=0;
    uint64_t qpc=0;
    uint64_t sequence=0;
    uint32_t bits[33]{};
    double physical[12]{}; // published position, linear/angular velocity; last 3 reserved
};
struct Lane {
    std::atomic<DWORD> owner{0};
    std::atomic<uint32_t> write{0},read{0};
    uint64_t sequence=0;
    Record records[kQueue];
};
Lane g_lanes[kLanes];
struct Port {
    uintptr_t pointer=0;
    uint32_t kind=0,native_index=0;
    uint32_t history[4][33]{};
    uint32_t last[4]{};
    bool valid[4]{};
};
struct Component {
    uintptr_t pointer=0,definition=0;
    Identity id{};
    uint32_t seen=0,nports=0,revision=0;
    Port ports[kPorts];
};
Component g_cache[kComponents];
std::atomic<DWORD> g_component_owner[kComponents]{};
std::atomic<uint64_t> g_conflicts{0};
uint32_t g_active[kComponents],g_nactive=0,g_generation=0;
uint32_t g_revision=0;
struct Body { uintptr_t pointer; Identity id; };
Body g_bodies[kBodies];
uint32_t g_nbodies=0;
std::atomic<bool> g_enabled{false},g_in_tick{false};
std::atomic_flag g_tick_owner=ATOMIC_FLAG_INIT;
std::atomic<uint64_t> g_busy{0};
std::atomic<uint64_t> g_dropped{0},g_faults{0},g_truncated{0},g_unsupported{0},g_written{0},g_max_us{0};
std::atomic<uint32_t> g_bridge_batches{0},g_component_calls{0};
std::atomic<uint32_t> g_selected_components{0},g_selected_bodies{0};
uint32_t g_tick=0,g_role=0,g_vehicle=0,g_every=1,g_keyframe=60;
bool g_capture=false;
uint32_t g_capture_vehicle=0;
std::atomic<uint32_t> g_capture_tick{0},g_capture_role{0};
std::atomic<uint64_t> g_correction_budget{UINT64_MAX};
std::atomic<uint64_t> g_sent{0},g_received{0},g_targets{0},g_suppressed{0},g_unmapped{0};
struct CatalogBody {uintptr_t body,scene;Identity id;};
CatalogBody g_catalog[512]{};
uint32_t g_catalog_count=0;
std::atomic_flag g_catalog_lock=ATOMIC_FLAG_INIT;
uint64_t g_limit_bytes=256ull<<20;
bool g_all=false,g_batch_seen=false;
uint32_t g_mode=UINT32_MAX,g_reason=UINT32_MAX;
size_t g_world_ring=0,g_slots=0;
uintptr_t g_world_list=0;
uintptr_t g_train=0,g_logic=0,g_mc=0,g_slot_vt[6]{};
LARGE_INTEGER g_frequency{};
wchar_t g_path[MAX_PATH]{};
thread_local int tl_lane=-1;
thread_local uint32_t tl_depth=0;
thread_local bool tl_owned=false;

template<class T> T rd(uintptr_t p) { return *reinterpret_cast<const T*>(p); }
struct Ring { uintptr_t data; uint32_t cap,head,n; };
bool ring(uintptr_t p,Ring& r,uint32_t max)
{
    r={rd<uintptr_t>(p),rd<uint32_t>(p+8),rd<uint32_t>(p+12),rd<uint32_t>(p+16)};
    return r.n<=max && r.n<=r.cap && (!r.n || (r.data && r.cap && r.head<r.cap));
}
uintptr_t at(const Ring& r,uint32_t i) { return rd<uintptr_t>(r.data+8*((uint64_t(r.head)+i)%r.cap)); }
uint64_t clock_qpc() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return uint64_t(q.QuadPart); }
void cost(uint64_t begin)
{
    uint64_t us=(clock_qpc()-begin)*1000000/uint64_t(g_frequency.QuadPart),old=g_max_us.load();
    while (us>old && !g_max_us.compare_exchange_weak(old,us)) {}
}
void submit(Record& record)
{
    if (tl_lane<0) {
        const DWORD me=GetCurrentThreadId();
        for (uint32_t i=0;i<kLanes;++i) {
            DWORD empty=0;
            if (g_lanes[i].owner.compare_exchange_strong(empty,me)) { tl_lane=int(i); break; }
        }
        if (tl_lane<0) { g_dropped++; return; }
    }
    Lane& lane=g_lanes[tl_lane];
    record.sequence=++lane.sequence;
    uint32_t w=lane.write.load(std::memory_order_relaxed),r=lane.read.load(std::memory_order_acquire);
    if (w-r==kQueue) { g_dropped++; return; }
    record.qpc=clock_qpc();
    lane.records[w%kQueue]=record;
    lane.write.store(w+1,std::memory_order_release);
}
Record base(uint32_t kind,uint32_t stage,const Identity& id)
{
    Record r; r.kind=kind; r.stage=stage; r.id=id; r.tick=g_tick; r.role=g_role;r.unit=UINT32_MAX;return r;
}
Component* find(uintptr_t p)
{
    uint32_t lo=0,hi=g_nactive;
    while (lo<hi) { uint32_t m=lo+(hi-lo)/2; if(g_cache[g_active[m]].pointer<p)lo=m+1;else hi=m; }
    return lo<g_nactive && g_cache[g_active[lo]].pointer==p?&g_cache[g_active[lo]]:nullptr;
}
// Leaf native-read boundary. The component is live at its native caller; a
// disappearance during a whole-tick snapshot becomes a counted missing sample.
bool read_port(const Port& p,uint32_t* out,uint32_t& count)
{
    __try {
        count=p.kind>=4?33:1;
        if(p.kind==0 || p.kind==1) out[0]=rd<uint8_t>(p.pointer+0x18);
        else memcpy(out,reinterpret_cast<void*>(p.pointer+0x18),count*4);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void component_sample(Component& c,uint32_t stage,uint32_t unit,uint32_t order)
{
    for(uint32_t i=0;i<c.nports;++i) {
        Port& p=c.ports[i]; uint32_t words[33]{},n=0;
        if(!read_port(p,words,n)) { g_faults++; continue; }
        // Lossless run-length encoding per stage; a periodic keyframe anchors
        // reconstruction. The raw IEEE-754 bits preserve NaNs and signed zero.
        bool changed=!p.valid[stage] || g_tick-p.last[stage]>=g_keyframe || memcmp(words,p.history[stage],n*4)!=0;
        if(!changed)continue;
        Record r=base(SlotRecord,stage,c.id);r.port=p.native_index;r.port_type=p.kind;r.count=n;r.unit=unit;r.order=order;
        memcpy(r.bits,words,n*4); submit(r);
        memcpy(p.history[stage],words,n*4);p.last[stage]=g_tick;p.valid[stage]=true;
    }
}
bool body_values(uintptr_t b,Record& r)
{
    __try {
        uintptr_t s=rd<uintptr_t>(b+0x718);
        if(!s || rd<uintptr_t>(s+0xc0)!=b+0x260)return false;
        memcpy(r.physical,reinterpret_cast<void*>(s+0x78),24);
        for(int i=0;i<3;++i) {r.physical[3+i]=rd<float>(s+0x98+i*4);r.physical[6+i]=rd<float>(s+0xa4+i*4);}
        // The native physics job can still be in flight at a simulation return.
        // Read its published scene cache only, never the live Bullet velocity,
        // constraints or activation state from the simulation observer.
        r.bits[0]=rd<uint32_t>(s+0x14);r.bits[1]=r.bits[2]=UINT32_MAX;
        r.bits[3]=rd<uint32_t>(b+0x218);r.bits[4]=rd<uint8_t>(b+0x21c);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
void body_sample(const Body& b,uint32_t stage)
{
    Record r=base(BodyRecord,stage,b.id);
    if(body_values(b.pointer,r))submit(r);else g_faults++;
}

Component* cache(uintptr_t c,uintptr_t def,const Identity& id)
{
    Component* choice=nullptr;
    for(auto& old:g_cache) {
        if(old.pointer==c) { choice=&old;break; }
        if(!choice && old.seen!=g_generation)choice=&old;
    }
    if(!choice)return nullptr;
    if(choice->pointer!=c || choice->definition!=def || memcmp(&choice->id,&id,sizeof id)!=0) {
        *choice={};choice->pointer=c;choice->definition=def;choice->id=id;
        choice->revision=++g_revision;
    }
    choice->seen=g_generation;
    return choice;
}
bool is_train(uintptr_t vehicle,uint32_t& scanned)
{
    Ring bodies;if(!ring(vehicle,bodies,512))return false;
    for(uint32_t bi=0;bi<bodies.n;++bi) {
        if(++scanned>40000)return false;
        uintptr_t body=at(bodies,bi);Ring components;
        if(!body || !ring(body+0x450,components,20000))continue;
        for(uint32_t ci=0;ci<components.n;++ci) {
            if(++scanned>40000)return false;
            uintptr_t c=at(components,ci);if(c && rd<uintptr_t>(c)==g_train)return true;
        }
    }
    return false;
}
bool prepare(uintptr_t simulation,uintptr_t player)
{
    __try {
        g_tick=rd<uint32_t>(player+0x33c);g_nactive=g_nbodies=0;
        if(g_tick%g_every)return true;
        ++g_generation;
        uintptr_t world=rd<uintptr_t>(simulation);Ring vehicles;
        if(!world || !ring(world+g_world_ring,vehicles,16384))return false;
        g_world_list=world+(game_build()==0x6a9a9e20?0x16a68:0x16a70);
        uint32_t scanned=0,total_ports=0;
        for(uint32_t vi=0;vi<vehicles.n;++vi) {
            if(++scanned>40000) {g_truncated++;break;}
            uintptr_t entry=at(vehicles,vi);
            if(!entry || rd<uint8_t>(entry) || rd<uint32_t>(entry+0x10)!=5)continue;
            uint32_t id=rd<uint32_t>(entry+4);uintptr_t vehicle=rd<uintptr_t>(entry+8);
            if(!vehicle || (g_vehicle?id!=g_vehicle:!g_all && !is_train(vehicle,scanned))) {
                if(scanned>40000) {g_truncated++;break;}
                continue;
            }
            Ring bodies;if(!ring(vehicle,bodies,512))return false;
            for(uint32_t bi=0;bi<bodies.n;++bi) {
                uintptr_t b=at(bodies,bi);if(!b)continue;
                Identity body_id{};body_id.vehicle=id;body_id.body=bi;
                logic_trace_catalog_body(b,rd<uintptr_t>(b+0x718),id,bi);
                if(g_nbodies<kBodies)g_bodies[g_nbodies++]={b,body_id};else g_truncated++;
                Ring components;if(!ring(b+0x450,components,20000))return false;
                for(uint32_t ci=0;ci<components.n;++ci) {
                    if(++scanned>40000) {g_truncated++;return true;}
                    uintptr_t c=at(components,ci);if(!c)continue;
                    if(!native_object_is_a(reinterpret_cast<void*>(c),"c_scene_vehicle_component_extended_base"))continue;
                    Ring slots;if(!ring(c+g_slots,slots,1024)) {g_faults++;continue;}
                    if(!slots.n)continue;
                    if(g_nactive==kComponents) {g_truncated++;continue;}
                    Identity key=body_id;memcpy(key.voxel,reinterpret_cast<void*>(c+0x18),sizeof key.voxel);
                    uintptr_t vt=rd<uintptr_t>(c),def=rd<uintptr_t>(c+0x58);
                    key.component_type=vt==g_mc?1000:vt==g_train?1001:vt==g_logic && def?rd<uint32_t>(def+0xe70):1002;
                    Component* out=cache(c,def,key);if(!out) {g_truncated++;continue;}
                    const uint32_t old_count=out->nports;out->nports=0;bool changed=false;
                    for(uint32_t si=0;si<slots.n;++si) {
                        uintptr_t slot=at(slots,si);if(!slot)continue;
                        uintptr_t svt=rd<uintptr_t>(slot);uint32_t type=0;
                        while(type<6 && g_slot_vt[type]!=svt)++type;
                        if(type==6) {g_unsupported++;continue;}
                        if(total_ports==kTotalPorts) {g_truncated++;break;}
                        if(out->nports==kPorts) {g_truncated++;break;}
                        Port& p=out->ports[out->nports++];
                        ++total_ports;
                        if(p.pointer!=slot || p.kind!=type || p.native_index!=si) {p={};p.pointer=slot;p.kind=type;p.native_index=si;changed=true;}
                    }
                    if(changed || out->nports!=old_count) {out->revision=++g_revision;for(auto& p:out->ports)memset(p.valid,0,sizeof p.valid);}
                    g_active[g_nactive++]=uint32_t(out-g_cache);
                }
            }
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
void batch(uintptr_t list,uint32_t mode,uint32_t reason,uint32_t vehicles,uint32_t units)
{
    if(!g_in_tick.load(std::memory_order_acquire) || list!=g_world_list)return;
    g_batch_seen=true;g_bridge_batches++;
    Record r=base(BatchRecord,0,{});r.bits[0]=mode;r.bits[1]=reason;r.bits[2]=vehicles;r.bits[3]=units;
    r.bits[4]=g_mode!=mode || g_reason!=reason;g_mode=mode;g_reason=reason;submit(r);
}
void component(uintptr_t p,uint32_t stage,uint32_t unit,uint32_t order)
{
    if(!g_in_tick.load(std::memory_order_acquire) || stage<1 || stage>2)return;
    if(auto c=find(p)) {
        const uint32_t index=uint32_t(c-g_cache);const DWORD thread=GetCurrentThreadId();
        DWORD owner=0;
        bool own=stage==1?g_component_owner[index].compare_exchange_strong(owner,thread):g_component_owner[index].load()==thread;
        if(!own)g_conflicts++;
        Record r=base(ComponentRecord,stage,c->id);r.unit=unit;r.order=order;r.bits[0]=thread;r.bits[1]=own?0:1;submit(r);
        g_component_calls++;
        if(own)component_sample(*c,stage,unit,order);
        if(stage==2 && own)g_component_owner[index].store(0);
    }
}
void body(uintptr_t p,uint32_t stage)
{
    if(!g_in_tick.load(std::memory_order_acquire))return;
    for(uint32_t i=0;i<g_nbodies;++i)if(g_bodies[i].pointer==p) {body_sample(g_bodies[i],stage);break;}
}
const LogicTraceBridge g_bridge={sizeof(LogicTraceBridge),1,&batch,&component,&body};

void number(FILE* f,double x) {if(std::isfinite(x))fprintf(f,"%.12g",x);else fputs("null",f);}
void write(FILE* f,const Record& r,uint32_t lane)
{
    fprintf(f,"{\"kind\":%u,\"tick\":%u,\"role\":%u,\"stage\":%u,\"vehicle\":%u,\"body\":%u,\"voxel\":[%d,%d,%d],\"type\":%u,\"lane\":%u,\"seq\":%llu,\"qpc\":%llu,\"unit\":%u,\"order\":%u",
        r.kind,r.tick,r.role,r.stage,r.id.vehicle,r.id.body,r.id.voxel[0],r.id.voxel[1],r.id.voxel[2],r.id.component_type,lane,r.sequence,r.qpc,r.unit,r.order);
    if(r.kind==SlotRecord) {
        fprintf(f,",\"port\":%u,\"port_type\":%u,\"words\":[",r.port,r.port_type);
        for(uint32_t i=0;i<r.count;++i)fprintf(f,"%s%u",i?",":"",r.bits[i]);fputc(']',f);
    } else if(r.kind==BodyRecord) {
        fputs(",\"physical\":[",f);for(int i=0;i<12;++i) {if(i)fputc(',',f);number(f,r.physical[i]);}
        fprintf(f,"],\"state_scope\":\"published_scene\",\"scene_flags\":%u,\"reach\":%u,\"target_valid\":%u",r.bits[0],r.bits[3],r.bits[4]);
    } else if(r.kind==InventoryRecord) {
        fprintf(f,",\"revision\":%u,\"ports\":[",r.port);
        for(uint32_t i=0;i<r.count;++i)fprintf(f,"%s[%u,%u]",i?",":"",r.bits[i]&0xffff,r.bits[i]>>16);
        fputc(']',f);
    } else if(r.kind==BatchRecord) fprintf(f,",\"mode\":%u,\"reason\":%u,\"loaded\":%u,\"units\":%u,\"transition\":%u",r.bits[0],r.bits[1],r.bits[2],r.bits[3],r.bits[4]);
    else if(r.kind==ComponentRecord) fprintf(f,",\"thread\":%u,\"capture_conflict\":%u",r.bits[0],r.bits[1]);
    else if(r.kind==CorrectionRecord) {
        fprintf(f,",\"peer\":%u,\"reach\":%u,\"motion_kind\":%u,\"body_count\":%u,\"quaternion_words\":[%u,%u,%u,%u],\"position\":[",r.port,r.bits[0],r.bits[1],r.count,r.bits[2],r.bits[3],r.bits[4],r.bits[5]);
        for(int i=0;i<3;++i){if(i)fputc(',',f);number(f,r.physical[i]);}fputc(']',f);
    } else if(r.kind==MessageRecord) fprintf(f,",\"peer\":%u,\"reach\":%u,\"body_count\":%u,\"payload_hash\":\"%08x%08x\",\"detailed\":%u",r.port,r.bits[0],r.count,r.bits[2],r.bits[1],r.bits[3]);
    else if(r.kind==TargetRecord) {
        fprintf(f,",\"reach\":%u,\"target_valid\":%u,\"matrix\":[",r.port,r.bits[32]);
        for(int i=0;i<16;++i){double x;memcpy(&x,r.bits+2*i,8);if(i)fputc(',',f);number(f,x);}fputc(']',f);
    }
    else fprintf(f,",\"components\":%u,\"bodies\":%u,\"batch_seen\":%u",r.bits[0],r.bits[1],r.bits[2]);
    fputs("}\n",f);
}
void writer()
{
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    FILE* file=nullptr;_wfopen_s(&file,g_path,L"wb");
    if(!file) {g_enabled=false;swlog("logic trace: cannot open output file");return;}
    static char buffer[1<<20];setvbuf(file,buffer,_IOFBF,sizeof buffer);
    fprintf(file,"{\"schema\":2,\"build\":%u,\"qpc_frequency\":%llu,\"every_ticks\":%u,\"keyframe_ticks\":%u,\"combined_capture\":%s,\"correction_vehicle\":%u,\"encoding\":\"u32 IEEE754; composite bool mask then 32 floats\"}\n",game_build(),uint64_t(g_frequency.QuadPart),g_every,g_keyframe,g_capture?"true":"false",g_capture_vehicle);
    DWORD last=GetTickCount(),health=last;
    for(;;) {
        for(uint32_t i=0;i<kLanes;++i) {
            Lane& lane=g_lanes[i];uint32_t r=lane.read.load(std::memory_order_relaxed),w=lane.write.load(std::memory_order_acquire);
            while(r!=w) {Record rec=lane.records[r%kQueue];lane.read.store(++r,std::memory_order_release);write(file,rec,i);g_written++;}
        }
        if(GetTickCount()-last>=1000) {
            last=GetTickCount();
            fprintf(file,"{\"status\":true,\"dropped\":%llu,\"faults\":%llu,\"truncated\":%llu,\"busy\":%llu,\"unsupported_slots\":%llu,\"written\":%llu,\"batch_callbacks\":%u,\"component_callbacks\":%u,\"max_boundary_us\":%llu}\n",
                g_dropped.load(),g_faults.load(),g_truncated.load(),g_busy.load(),g_unsupported.load(),g_written.load(),g_bridge_batches.load(),g_component_calls.load(),g_max_us.load());
            fprintf(file,"{\"correction_status\":true,\"sent\":%llu,\"received\":%llu,\"targets\":%llu,\"suppressed\":%llu,\"unmapped\":%llu,\"component_conflicts\":%llu}\n",g_sent.load(),g_received.load(),g_targets.load(),g_suppressed.load(),g_unmapped.load(),g_conflicts.load());
            if(g_capture && last-health>=10000) {
                health=last;
                swlog("capture health: selected %u components/%u bodies; batch=%u component=%u sent=%llu received=%llu target=%llu suppressed=%llu; dropped=%llu faults=%llu truncated=%llu conflicts=%llu; %s",
                    g_selected_components.load(),g_selected_bodies.load(),g_bridge_batches.load(),g_component_calls.load(),g_sent.load(),g_received.load(),g_targets.load(),g_suppressed.load(),g_dropped.load(),g_faults.load(),g_truncated.load(),g_conflicts.load(),
                    !g_selected_components.load()?"train not selected yet (spawn it or set logic_trace.vehicle_id)":!g_component_calls.load()?"MISSING component adapter callbacks; whole-tick snapshots only":"component adapter observed; check trace coverage counters");
            }
            if(fflush(file)!=0 || ferror(file)) {g_enabled=false;swlog("logic trace: output failed; capture disabled");fclose(file);return;}
            if(uint64_t(_ftelli64(file))>=g_limit_bytes) {g_enabled=false;swlog("logic trace: file size limit reached; capture stopped (%ls)",g_path);fclose(file);return;}
        }
        Sleep(10);
    }
}
}

extern "C" __declspec(dllexport) const LogicTraceBridge* __cdecl swml_logic_trace_bridge()
{
    return g_enabled.load(std::memory_order_acquire)?&g_bridge:nullptr;
}
bool logic_trace_install()
{
    g_capture=GetPrivateProfileIntW(L"capture",L"enabled",0,settings_ini())!=0;
    if(!g_capture && !GetPrivateProfileIntW(L"logic_trace",L"enabled",0,settings_ini()))return false;
    g_capture_vehicle=GetPrivateProfileIntW(L"capture",L"vehicle_id",0,settings_ini());
    if((game_build()!=0x6a9a9e20 && game_build()!=0x6ae31989) || !resolve(kResolveScope)) {
        swlog("logic trace: unavailable (unverified game layout or SDK slot discovery failed)");return false;
    }
    g_slots=addrs().off_scope_slots;
    g_train=native_find_vtable("c_scene_vehicle_component_train");g_logic=native_find_vtable("c_scene_vehicle_component_logic_gate");g_mc=native_find_vtable("c_scene_vehicle_component_microprocessor");
    const char* names[]={"c_vehicle_logic_slot_input_bool","c_vehicle_logic_slot_output_bool","c_vehicle_logic_slot_input_f32","c_vehicle_logic_slot_output_f32","c_vehicle_logic_slot_input_composite","c_vehicle_logic_slot_output_composite"};
    for(int i=0;i<6;++i)if(!(g_slot_vt[i]=native_find_vtable(names[i]))) {swlog("logic trace: missing slot class %s",names[i]);return false;}
    const uint8_t setter[]={0xf3,0x0f,0x11,0x49,0x18,0xc3};
    if(memcmp(reinterpret_cast<void*>(rd<uintptr_t>(g_slot_vt[2]+11*8)),setter,sizeof setter)!=0) {
        swlog("logic trace: numeric slot storage contract changed; capture unavailable");return false;
    }
    if(!g_train || !g_logic || !g_mc || !g_slots) {swlog("logic trace: missing native component/slot discovery");return false;}
    g_world_ring=game_build()==0x6a9a9e20?0x4ca8:0x4cb0;
    g_vehicle=GetPrivateProfileIntW(L"logic_trace",L"vehicle_id",0,settings_ini());g_all=GetPrivateProfileIntW(L"logic_trace",L"all_vehicles",0,settings_ini())!=0;
    g_every=g_capture?1:std::clamp(int(GetPrivateProfileIntW(L"logic_trace",L"every_ticks",1,settings_ini())),1,60);
    g_limit_bytes=uint64_t(std::clamp(int(GetPrivateProfileIntW(L"logic_trace",L"max_mb",256,settings_ini())),16,2048))<<20;
    if(g_capture)g_limit_bytes=uint64_t(std::clamp(int(GetPrivateProfileIntW(L"capture",L"max_mb",1024,settings_ini())),16,2048))<<20;
    QueryPerformanceFrequency(&g_frequency);SYSTEMTIME now;GetSystemTime(&now);
    int n=swprintf_s(g_path,L"%s\\logic_trace_%04u%02u%02uT%02u%02u%02uZ_%lu.jsonl",log_dir(),now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,GetCurrentProcessId());
    if(n<0)return false;
    g_enabled=true;std::thread(writer).detach();
    if(g_capture)swlog("combined capture: ON; corrections for all vehicles hashed; detailed camera/train catalog or capture.vehicle_id=%u; target stages 2=input,3=stored,4=scene forwarded,5=scene suppressed; missing data is not a clean result",g_capture_vehicle);
    swlog("logic trace: on (every %u host ticks, vehicle_id=%u, auto trains=%d); file %ls; Physics Fixes component adapter required for pre/post-component detail",g_every,g_vehicle,!g_all && !g_vehicle,g_path);
    return true;
}
void logic_trace_begin(uintptr_t simulation,uintptr_t player,bool host)
{
    if(tl_depth) {++tl_depth;return;}
    if(!g_enabled || !simulation || !player)return;
    tl_depth=1;
    if(g_tick_owner.test_and_set(std::memory_order_acquire)) {tl_owned=false;g_busy++;return;}
    tl_owned=true;
    g_in_tick.store(false,std::memory_order_release);uint64_t begin=clock_qpc();g_role=host?1:0;g_batch_seen=false;
    if(!prepare(simulation,player)) {g_faults++;g_enabled=false;swlog("logic trace: native world read failed; disabled");return;}
    g_capture_tick.store(g_tick);g_capture_role.store(g_role);
    g_selected_components.store(g_nactive);g_selected_bodies.store(g_nbodies);
    std::sort(g_active,g_active+g_nactive,[](uint32_t a,uint32_t b){return g_cache[a].pointer<g_cache[b].pointer;});
    if(g_tick%g_every)return;
    for(uint32_t i=0;i<g_nactive;++i) {
        Component& c=g_cache[g_active[i]];
        for(uint32_t start=0;start<c.nports;start+=32) {
            Record inventory=base(InventoryRecord,0,c.id);inventory.port=c.revision;
            inventory.count=std::min(32u,c.nports-start);
            for(uint32_t j=0;j<inventory.count;++j){const Port& p=c.ports[start+j];inventory.bits[j]=p.native_index | (p.kind<<16);}
            submit(inventory);
        }
        component_sample(c,0,UINT32_MAX,0);
    }
    for(uint32_t i=0;i<g_nbodies;++i)body_sample(g_bodies[i],0);
    Record r=base(TickRecord,0,{});r.bits[0]=g_nactive;r.bits[1]=g_nbodies;submit(r);
    g_in_tick.store(true,std::memory_order_release);cost(begin);
}
void logic_trace_end()
{
    if(!tl_depth || --tl_depth || !tl_owned)return;
    tl_owned=false;
    struct Leave {~Leave(){g_tick_owner.clear(std::memory_order_release);}} leave;
    if(!g_in_tick.exchange(false,std::memory_order_acq_rel))return;
    uint64_t begin=clock_qpc();
    for(uint32_t i=0;i<g_nactive;++i)component_sample(g_cache[g_active[i]],3,UINT32_MAX,0);
    for(uint32_t i=0;i<g_nbodies;++i)body_sample(g_bodies[i],3);
    Record r=base(TickRecord,3,{});r.bits[0]=g_nactive;r.bits[1]=g_nbodies;r.bits[2]=g_batch_seen;submit(r);cost(begin);
}

// Append-only catalog: never dereference retained native pointers. A reused
// pointer is updated under the same try-lock as readers; contention loses a
// diagnostic sample and is counted rather than delaying a game thread.
void logic_trace_catalog_body(uintptr_t body,uintptr_t scene,uint32_t vehicle,uint32_t index)
{
    if(!g_enabled || !g_capture)return;
    if(g_catalog_lock.test_and_set(std::memory_order_acquire)){g_busy++;return;}
    uint32_t i=0;while(i<g_catalog_count && g_catalog[i].body!=body)++i;
    if(i==512)g_truncated++;
    else {if(i==g_catalog_count)++g_catalog_count;g_catalog[i]={body,scene,{vehicle,index,{0,0,0},0}};}
    g_catalog_lock.clear(std::memory_order_release);
}
namespace {
bool catalog(uintptr_t pointer,bool scene,uint32_t vehicle,Identity& out)
{
    if(g_catalog_lock.test_and_set(std::memory_order_acquire)){g_busy++;return false;}
    bool found=false;
    for(uint32_t i=0;i<g_catalog_count;++i) {
        const auto& c=g_catalog[i];
        if(pointer?(scene?c.scene:c.body)==pointer:c.id.vehicle==vehicle){out=c.id;found=true;break;}
    }
    g_catalog_lock.clear(std::memory_order_release);return found;
}
Record capture_record(uint32_t kind,uint32_t stage,const Identity& id,uint32_t tick)
{
    Record r;r.kind=kind;r.stage=stage;r.id=id;r.tick=tick;r.role=g_capture_role.load();r.unit=UINT32_MAX;return r;
}
}
void logic_trace_correction(uintptr_t message,uint32_t peer,bool sending,uint32_t tick)
{
    if(!g_enabled || !g_capture || !message)return;
    __try {
        if(rd<uint32_t>(message+8)!=0x81)return;
        const uint32_t vehicle=rd<uint32_t>(message+0x10),reach=rd<uint32_t>(message+0x14),n=rd<uint32_t>(message+0x20);
        const uintptr_t data=rd<uintptr_t>(message+0x18);
        if(n>4096 || (n && !data)){g_faults++;return;}
        const uint32_t stamp=sending?g_capture_tick.load():tick;
        // Fixed total read/hash budget across all peers in a host tick. Even a
        // malformed or enormous world must not turn diagnostics into a long wait.
        uint64_t budget=g_correction_budget.load();bool reserved=false;
        for(int attempt=0;attempt<4;++attempt) {
            uint32_t used=uint32_t(budget>>32)==stamp?uint32_t(budget):0;
            if(used+n>8192){g_truncated+=n;return;}
            if(g_correction_budget.compare_exchange_weak(budget,(uint64_t(stamp)<<32)|(used+n))){reserved=true;break;}
        }
        if(!reserved){g_busy++;return;}
        Identity id{};id.vehicle=vehicle;
        const bool detail=g_capture_vehicle?vehicle==g_capture_vehicle:catalog(0,false,vehicle,id);
        uint64_t hash=14695981039346656037ull;
        // Hash canonical wire precision, never native padding or float->double
        // expansion. Kind 0 has no pose; kind 2 transmits float positions.
        for(uint32_t i=0;i<n;++i) {
            uintptr_t p=data+size_t(i)*0x30;uint8_t canonical[41]{};
            uint8_t kind=rd<uint8_t>(p+0x28);canonical[0]=kind;
            if(kind>2){g_faults++;return;}
            size_t bytes=1;
            if(kind) {
                memcpy(canonical+1,reinterpret_cast<void*>(p),16);bytes+=16;
                for(int k=0;k<3;++k) {
                    double value=rd<double>(p+0x10+8*k);
                    if(kind==1){memcpy(canonical+bytes,&value,8);bytes+=8;}
                    else {float value32=float(value);memcpy(canonical+bytes,&value32,4);bytes+=4;}
                }
            }
            for(size_t b=0;b<bytes;++b){hash^=canonical[b];hash*=1099511628211ull;}
            if(detail && i<512) {
                id.body=i;Record r=capture_record(CorrectionRecord,sending?0:1,id,sending?g_capture_tick.load():tick);
                r.port=peer;r.count=n;r.bits[0]=reach;r.bits[1]=kind;
                if(kind){memcpy(r.bits+2,canonical+1,16);for(int k=0;k<3;++k){double v=rd<double>(p+0x10+8*k);r.physical[k]=kind==2?double(float(v)):v;}}
                submit(r);
            }
        }
        if(detail && n>512)g_truncated+=n-512;
        id.body=0;Record r=capture_record(MessageRecord,sending?0:1,id,sending?g_capture_tick.load():tick);
        r.port=peer;r.count=n;r.bits[0]=reach;r.bits[1]=uint32_t(hash);r.bits[2]=uint32_t(hash>>32);r.bits[3]=detail;submit(r);
        if(sending)g_sent++;else g_received++;
    } __except(EXCEPTION_EXECUTE_HANDLER){g_faults++;}
}
void logic_trace_received(uintptr_t list,uintptr_t player)
{
    if(!g_enabled || !g_capture || !list || !player)return;
    __try {
        Ring messages;if(!ring(list,messages,100000)){g_faults++;return;}
        const uint32_t tick=rd<uint32_t>(player+0x33c);
        const uint32_t n=std::min(messages.n,8192u);if(n!=messages.n)g_truncated+=messages.n-n;
        for(uint32_t i=0;i<n;++i)logic_trace_correction(at(messages,i),UINT32_MAX,false,tick);
    } __except(EXCEPTION_EXECUTE_HANDLER){g_faults++;}
}
void logic_trace_target(uintptr_t body,uint32_t reach,const double* transform,uint32_t stage,uintptr_t player)
{
    if(!g_enabled || !g_capture || !body || !transform)return;
    Identity id{};if(!catalog(body,false,0,id)){g_unmapped++;return;}
    __try {
        Record r=capture_record(TargetRecord,stage,id,player?rd<uint32_t>(player+0x33c):g_capture_tick.load());
        r.port=stage==3?rd<uint32_t>(body+0x218):reach;r.bits[32]=stage==3?rd<uint8_t>(body+0x21c):0;
        memcpy(r.bits,transform,16*sizeof(double));submit(r);g_targets++;
    } __except(EXCEPTION_EXECUTE_HANDLER){g_faults++;}
}
void logic_trace_scene(uintptr_t scene,const double* transform,bool suppressed)
{
    if(!g_enabled || !g_capture || !scene || !transform)return;
    Identity id{};if(!catalog(scene,true,0,id)){g_unmapped++;return;}
    __try {
        Record r=capture_record(TargetRecord,suppressed?5:4,id,g_capture_tick.load());
        memcpy(r.bits,transform,16*sizeof(double));submit(r);if(suppressed)g_suppressed++;
    } __except(EXCEPTION_EXECUTE_HANDLER){g_faults++;}
}
