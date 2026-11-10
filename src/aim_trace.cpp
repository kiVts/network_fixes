// Issue #1: distinguish rigid-body replication from internal laser/camera aim.
// Fixed-size snapshots only on the simulation thread; formatting/disk logging is
// on a consumer thread. Full queue => drop diagnostic records, never wait.
#include "aim_trace.h"
#include "aim_trace_contract.h"
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "resolve.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
struct Record {
    uint32_t tick, vehicle, body, reach;
    int voxel[3], mount[9];
    uint8_t host, camera, pose_valid, target_valid, stabilized, tracking;
    float angle[2], input[2]; // radians; native rotation input values
    double pose[16], target[16], ray_end[3];
};
constexpr uint32_t kSlots=1024, kBatch=64;
Record g_records[kSlots];
std::atomic<uint32_t> g_write{0}, g_read{0};
std::atomic<uint64_t> g_dropped{0}, g_truncated{0}, g_faults{0}, g_samples{0}, g_found{0};
std::atomic<bool> g_on{false};
std::atomic_flag g_sampling=ATOMIC_FLAG_INIT;
std::atomic<uint64_t> g_busy_skips{0};
uint32_t g_every=6, g_vehicle=0, g_last_tick=0;
bool g_have_tick=false;
uintptr_t g_camera=0;
size_t g_vehicle_ring=0;

template<class T> T rd(uintptr_t p) { return *reinterpret_cast<const T*>(p); }
struct Ring { uintptr_t data; uint32_t cap, head, count; };
bool ring(uintptr_t p, Ring& r, uint32_t limit)
{
    r.data=rd<uintptr_t>(p); r.cap=rd<uint32_t>(p+8);
    r.head=rd<uint32_t>(p+12); r.count=rd<uint32_t>(p+16);
    return r.count<=limit && r.count<=r.cap && (!r.count || (r.data && r.cap && r.head<r.cap));
}
uintptr_t at(const Ring& r, uint32_t i) { return rd<uintptr_t>(r.data+8*((uint64_t(r.head)+i)%r.cap)); }

void body_record(Record& r, uintptr_t b, uint32_t tick, uint32_t vehicle, uint32_t index, bool host)
{
    r={}; r.tick=tick; r.vehicle=vehicle; r.body=index; r.host=host;
    const uintptr_t scene=rd<uintptr_t>(b+0x718);
    // Native scene object points back to this body's embedded scene owner.
    if (scene && rd<uintptr_t>(scene+0xc0)==b+0x260) {
        memcpy(r.pose,reinterpret_cast<void*>(scene+0x18),sizeof r.pose);
        r.pose_valid=1;
    }
    r.target_valid=rd<uint8_t>(b+0x21c)!=0;
    r.reach=rd<uint32_t>(b+0x218);
    if (r.target_valid) memcpy(r.target,reinterpret_cast<void*>(b+0x118),sizeof r.target);
}

bool collect(uintptr_t simulation, uintptr_t player, bool host, Record* out, uint32_t& n, bool& cut, uint32_t& tick)
{
    __try {
        tick=rd<uint32_t>(player+0x33c);
        uintptr_t world=rd<uintptr_t>(simulation);
        if (!world) return false;
        Ring vehicles;
        if (!ring(world+g_vehicle_ring,vehicles,16384)) return false;
        // The native host correction sender uses these exact per-peer positions
        // when choosing distance-dependent update intervals. Keep the host/local
        // flag so a proximity report can be checked against every observer.
        if(host) {
            Ring peers;
            if(!ring(player+0x1c0,peers,64))return false;
            for(uint32_t i=0;i<peers.count;++i) {
                if(n==kBatch) {cut=true;return true;}
                uintptr_t p=at(peers,i);if(!p)continue;
                Record& r=out[n++];r={};r.tick=tick;r.host=1;r.camera=2;
                r.body=rd<uint16_t>(p+0x80);r.target_valid=rd<uint8_t>(p+0xa0)!=0;
                memcpy(r.pose+12,reinterpret_cast<void*>(p+0x40),24);
            }
        }
        uint32_t scanned=0;
        for (uint32_t vi=0;vi<vehicles.count;++vi) {
            if (++scanned>20000) { cut=true; return true; }
            uintptr_t entry=at(vehicles,vi);
            if (!entry || rd<uint8_t>(entry) || rd<uint32_t>(entry+0x10)!=5) continue;
            uint32_t id=rd<uint32_t>(entry+4);
            if (g_vehicle && id!=g_vehicle) continue;
            uintptr_t v=rd<uintptr_t>(entry+8);
            Ring bodies;
            if (!v || !ring(v,bodies,512)) return false;
            const uint32_t start=n;
            for (uint32_t bi=0;bi<bodies.count;++bi) {
                if (++scanned>20000) { cut=true; return true; }
                uintptr_t body=at(bodies,bi);
                if (!body) continue;
                Ring components;
                if (!ring(body+0x450,components,100000)) return false;
                for (uint32_t ci=0;ci<components.count;++ci) {
                    if (++scanned>20000) { cut=true; return true; }
                    uintptr_t c=at(components,ci);
                    if (!c || rd<uintptr_t>(c)!=g_camera) continue;
                    if (n==kBatch) { cut=true; return true; }
                    Record& r=out[n++]; body_record(r,body,tick,id,bi,host);
                    r.camera=1;
                    memcpy(r.voxel,reinterpret_cast<void*>(c+0x18),sizeof r.voxel);
                    memcpy(r.mount,reinterpret_cast<void*>(c+0x30),sizeof r.mount);
                    r.angle[0]=rd<float>(c+0x67c); r.angle[1]=rd<float>(c+0x678);
                    r.input[0]=rd<float>(c+0x4b8); r.input[1]=rd<float>(c+0x498);
                    r.stabilized=rd<uint8_t>(c+0x6c1); r.tracking=rd<uint8_t>(c+0x6c2);
                    memcpy(r.ray_end,reinterpret_cast<void*>(c+0x6c8),sizeof r.ray_end);
                }
            }
            // Include every sub-body of a camera-bearing vehicle, not only the
            // body holding the laser: elevation pivots can be separate bodies.
            if (n!=start) for (uint32_t bi=0;bi<bodies.count;++bi) {
                if (n==kBatch) { cut=true; return true; }
                uintptr_t body=at(bodies,bi);
                if (body) body_record(out[n++],body,tick,id,bi,host);
            }
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool read_tick(uintptr_t p, uint32_t& t)
{
    __try { t=rd<uint32_t>(p+0x33c); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void matrix(char* out, size_t size, const double* m)
{
    size_t used=0;
    for (int i=0;i<16;++i) {
        const int n=snprintf(out+used,size-used,"%s%.9g",i?",":"",m[i]);
        if (n<0 || size_t(n)>=size-used) break;
        used+=size_t(n);
    }
}
void writer()
{
    uint32_t status=GetTickCount();
    for (;;) {
        uint32_t r=g_read.load(std::memory_order_relaxed);
        const uint32_t w=g_write.load(std::memory_order_acquire);
        while (r!=w) {
            const Record x=g_records[r%kSlots];
            g_read.store(++r,std::memory_order_release);
            if(x.camera==2) {
                swlog("aim peer: {\"tick\":%u,\"peer\":%u,\"local\":%u,\"position\":[%.9g,%.9g,%.9g]}",
                      x.tick,x.body,x.target_valid,x.pose[12],x.pose[13],x.pose[14]);
                continue;
            }
            char pose[640]={}, target[640]={};
            matrix(pose,sizeof pose,x.pose); matrix(target,sizeof target,x.target);
            swlog("aim trace: {\"tick\":%u,\"role\":\"%s\",\"vehicle\":%u,\"body\":%u,\"kind\":\"%s\","
                  "\"voxel\":[%d,%d,%d],\"mount\":[%d,%d,%d,%d,%d,%d,%d,%d,%d],\"angle\":[%.9g,%.9g],\"input\":[%.9g,%.9g],\"stabilized\":%u,\"tracking\":%u,"
                  "\"ray_end\":[%.9g,%.9g,%.9g],\"pose_valid\":%u,\"pose\":[%s],\"target_valid\":%u,\"reach\":%u,\"target\":[%s]}",
                  x.tick,x.host?"host":"client",x.vehicle,x.body,x.camera?"camera":"body",x.voxel[0],x.voxel[1],x.voxel[2],
                  x.mount[0],x.mount[1],x.mount[2],x.mount[3],x.mount[4],x.mount[5],x.mount[6],x.mount[7],x.mount[8],
                  x.angle[0],x.angle[1],x.input[0],x.input[1],x.stabilized,x.tracking,x.ray_end[0],x.ray_end[1],x.ray_end[2],
                  x.pose_valid,pose,x.target_valid,x.reach,target);
        }
        if (GetTickCount()-status>=5000) {
            status=GetTickCount();
            swlog("aim trace status: samples=%llu records=%llu dropped=%llu truncated=%llu faults=%llu busy_skips=%llu (cumulative)",
                  g_samples.load(),g_found.load(),g_dropped.load(),g_truncated.load(),g_faults.load(),g_busy_skips.load());
        }
        Sleep(100);
    }
}

bool contract(uintptr_t address, const uint8_t* bytes, size_t size)
{
    uintptr_t begin=0,end=0;
    return native_function_bounds(address,&begin,&end) && end>begin && end-begin<0x10000 &&
        aimtrace::contains(reinterpret_cast<const uint8_t*>(begin),end-begin,bytes,size);
}
}

bool aim_trace_install()
{
    if (!GetPrivateProfileIntW(L"aim_trace",L"enabled",0,settings_ini())) return false;
    // Deliberately limited to actual-PE-verified layouts. Future builds fail closed
    // even if a generic function signature happens to continue matching.
    if (game_build()!=0x6a9a9e20 && game_build()!=0x6ae31989) {
        swlog("aim trace: unavailable (build %x has no verified field contract)",game_build()); return false;
    }
    g_camera=native_find_vtable("c_scene_vehicle_component_camera");
    uintptr_t tick=g_camera?rd<uintptr_t>(g_camera+107*8):0;
    uintptr_t loop=game_live_if_mapped(0x140549240);
    if (!contract(tick,aimtrace::camera_angles,sizeof aimtrace::camera_angles) ||
        !contract(tick,aimtrace::camera_modes,sizeof aimtrace::camera_modes) ||
        !contract(tick,aimtrace::camera_input,sizeof aimtrace::camera_input) ||
        !contract(loop,aimtrace::component_ring,sizeof aimtrace::component_ring) ||
        !contract(loop,aimtrace::body_scene,sizeof aimtrace::body_scene)) {
        swlog("aim trace: unavailable (native camera/body field contract changed)"); return false;
    }
    g_vehicle_ring=game_build()==0x6a9a9e20?0x4ca8:0x4cb0;
    g_every=std::clamp(int(GetPrivateProfileIntW(L"aim_trace",L"every_ticks",6,settings_ini())),1,600);
    g_vehicle=GetPrivateProfileIntW(L"aim_trace",L"vehicle_id",0,settings_ini());
    g_on=true;
    std::thread(writer).detach();
    swlog("aim trace: on schema=1 every_ticks=%u vehicle_id=%u (0=all camera vehicles); end-of-tick radians, scene matrices; fixed 64-record sample / 1024-record queue",g_every,g_vehicle);
    return true;
}

void aim_trace_sample(uintptr_t simulation, uintptr_t player, bool host)
{
    if (!g_on || !simulation || !player) return;
    if (g_sampling.test_and_set(std::memory_order_acquire)) { g_busy_skips++; return; }
    struct Leave { ~Leave() { g_sampling.clear(std::memory_order_release); } } leave;
    uint32_t tick=0;
    if (!read_tick(player,tick)) { g_faults++; g_on=false; return; }
    if (tick%g_every || (g_have_tick && tick==g_last_tick)) return;
    g_have_tick=true; g_last_tick=tick;
    Record batch[kBatch]; uint32_t n=0; bool cut=false;
    if (!collect(simulation,player,host,batch,n,cut,tick)) { g_faults++; g_on=false; return; }
    g_samples++;
    g_found+=n;
    if (cut) g_truncated++;
    uint32_t w=g_write.load(std::memory_order_relaxed);
    const uint32_t r=g_read.load(std::memory_order_acquire);
    for (uint32_t i=0;i<n;++i) {
        if (w-r==kSlots) { g_dropped+=n-i; break; }
        g_records[w++%kSlots]=batch[i];
    }
    g_write.store(w,std::memory_order_release);
}
