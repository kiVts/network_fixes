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
enum Kind : uint32_t { SlotRecord=1,BodyRecord=2,BatchRecord=3,TickRecord=4,InventoryRecord=5,ComponentRecord=6 };
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
uint32_t g_tick=0,g_role=0,g_vehicle=0,g_every=1,g_keyframe=60;
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
        g_component_calls++;component_sample(*c,stage,unit,order);
        if(stage==2) {Record r=base(ComponentRecord,stage,c->id);r.unit=unit;r.order=order;submit(r);}
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
    else fprintf(f,",\"components\":%u,\"bodies\":%u,\"batch_seen\":%u",r.bits[0],r.bits[1],r.bits[2]);
    fputs("}\n",f);
}
void writer()
{
    FILE* file=nullptr;_wfopen_s(&file,g_path,L"wb");
    if(!file) {g_enabled=false;swlog("logic trace: cannot open output file");return;}
    static char buffer[1<<20];setvbuf(file,buffer,_IOFBF,sizeof buffer);
    fprintf(file,"{\"schema\":1,\"build\":%u,\"qpc_frequency\":%llu,\"every_ticks\":%u,\"keyframe_ticks\":%u,\"encoding\":\"u32 IEEE754; composite bool mask then 32 floats\"}\n",game_build(),uint64_t(g_frequency.QuadPart),g_every,g_keyframe);
    DWORD last=GetTickCount();
    for(;;) {
        for(uint32_t i=0;i<kLanes;++i) {
            Lane& lane=g_lanes[i];uint32_t r=lane.read.load(std::memory_order_relaxed),w=lane.write.load(std::memory_order_acquire);
            while(r!=w) {Record rec=lane.records[r%kQueue];lane.read.store(++r,std::memory_order_release);write(file,rec,i);g_written++;}
        }
        if(GetTickCount()-last>=1000) {
            last=GetTickCount();
            fprintf(file,"{\"status\":true,\"dropped\":%llu,\"faults\":%llu,\"truncated\":%llu,\"busy\":%llu,\"unsupported_slots\":%llu,\"written\":%llu,\"batch_callbacks\":%u,\"component_callbacks\":%u,\"max_boundary_us\":%llu}\n",
                g_dropped.load(),g_faults.load(),g_truncated.load(),g_busy.load(),g_unsupported.load(),g_written.load(),g_bridge_batches.load(),g_component_calls.load(),g_max_us.load());
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
    if(!GetPrivateProfileIntW(L"logic_trace",L"enabled",0,settings_ini()))return false;
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
    g_every=std::clamp(int(GetPrivateProfileIntW(L"logic_trace",L"every_ticks",1,settings_ini())),1,60);
    g_limit_bytes=uint64_t(std::clamp(int(GetPrivateProfileIntW(L"logic_trace",L"max_mb",256,settings_ini())),16,2048))<<20;
    QueryPerformanceFrequency(&g_frequency);SYSTEMTIME now;GetSystemTime(&now);
    int n=swprintf_s(g_path,L"%s\\logic_trace_%04u%02u%02uT%02u%02u%02uZ_%lu.jsonl",log_dir(),now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,GetCurrentProcessId());
    if(n<0)return false;
    g_enabled=true;std::thread(writer).detach();
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
