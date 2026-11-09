// First background-editor implementation. Off by default; mode 1 audits ownership, mode 2 enables the
// scheduling prototype ONLY for a vehicle editor with a distinct preview game world. Shared-world editors
// remain on the bounded inline fallback until their mutable preview/scene state can be separated.
// No scene cloning, forced shared-world concurrency, host changes, or dropping/reordering replayed ticks.
#include "editor_background.h"
#include "editor_replay.h"
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "resolve.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>

namespace {
using U = uintptr_t;
using Frame = void(__fastcall*)(U, U, U);
using Editor = void(__fastcall*)(U, U, U, U, U, U);
using Worker = void(__fastcall*)(U);
constexpr size_t kFront = 0x350, kCurrent = 0x358, kRequested = 0x35c, kSimulation = 0xce190;
constexpr size_t kAsyncAllowed = 0x330, kWorkerRunning = 0x331, kQueue = 0xce300;
constexpr size_t kEditorWorld = 0x68, kSimulationWorld = 0x38;
Frame g_frame = nullptr, g_transition = nullptr, g_shutdown = nullptr;
Editor g_editor = nullptr;
Worker g_start = nullptr, g_stop = nullptr;
U g_editor_vtable = 0, g_editor_entry = 0;
const volatile uint8_t* g_host = nullptr;
int g_mode = 0;
bool g_ready = false;
DWORD g_main_thread = 0;
editor_replay::Coordinator g_coordinator;
std::atomic<uint64_t> g_worker_rounds{0};
std::atomic<uint32_t> g_queue{0};
U g_observed_editor = 0;
U g_failed_editor = 0;
DWORD g_log_at = 0;
uint64_t g_frames = 0;

template<class T> T& field(U p, size_t off) { return *reinterpret_cast<T*>(p + off); }
bool changing(U game) { return field<int>(game, kCurrent) != field<int>(game, kRequested); }
bool connected(U data) { return field<U>(data + 0x280, 0x188) != 0; }
bool candidate(U game)
{
    U editor = field<U>(game, kFront);
    return editor && field<U>(editor, 0) == g_editor_vtable;
}
const char* refusal(editor_replay::Refusal reason)
{
    using R = editor_replay::Refusal;
    switch (reason) {
    case R::none: return "distinct preview world (static preflight only)";
    case R::missing_world: return "world ownership unavailable";
    case R::shared_world: return "preview and simulation share mutable game world; separation required";
    case R::async_disabled: return "native async updates disabled";
    case R::worker_owned: return "worker already owned by the game or another feature";
    case R::transition: return "foreground transition pending";
    }
    return "unknown";
}

// Worker entry state is published before the start call, and remains published until stop/join returns.
bool start_worker(U game)
{
    g_start(game);
    return field<uint8_t>(game, kWorkerRunning) != 0;
}
void stop_worker(U game) { g_stop(game); }
void stop(U game)
{
    if (!g_coordinator.owns(game)) return;
    g_coordinator.stop();
    swlog("editor background: worker joined before foreground transition / shutdown");
    g_observed_editor = 0;
    g_failed_editor = 0;
}

void __fastcall editor_detour(U editor, U application, U data, U state, U to_logic, U to_game)
{
    if (g_coordinator.suppress_editor(editor, GetCurrentThreadId())) {
        g_worker_rounds.fetch_add(1, std::memory_order_relaxed);
        // The game is still alive: stop/join retains ownership until this callback has returned.
        return;
    }
    g_editor(editor, application, data, state, to_logic, to_game);
}
void __fastcall transition_detour(U game, U application, U data)
{
    if (changing(game)) {
        stop(game); // the native code only joins when its OLD foreground was simulation
        g_failed_editor = g_observed_editor = 0;
    }
    g_transition(game, application, data);
}
void __fastcall shutdown_detour(U game, U application, U data)
{
    stop(game); // before any state exit callback, deletion or disconnect queue flush
    g_shutdown(game, application, data);
    g_observed_editor = 0;
    g_failed_editor = 0;
}
void __fastcall frame_detour(U game, U application, U data)
{
    g_main_thread = GetCurrentThreadId();
    if (g_coordinator.owns(game) && (*g_host || !candidate(game) || changing(game) || !connected(data) ||
                                    !field<uint8_t>(game, kWorkerRunning) || !field<uint8_t>(game, kAsyncAllowed)))
        stop(game);
    g_frame(game, application, data);
}

// Keep the prototype restricted to audited layouts, but discover every private entry point
// from a unique instruction pattern in executable sections rather than per-build addresses.
constexpr uint32_t kAuditedBuilds[] = {0x6a9a9e20, 0x6ae31989};
constexpr uint8_t kTransitionPrefix[] = {0x48,0x89,0x5c,0x24,8,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,
    0x57,0x48,0x83,0xec,0x30,0x8b,0x81,0x58,3,0,0,0x49,0x8b,0xf8,0x48,0x8b,0xea,0x48,0x8b,0xd9,0x39,0x81,0x5c,3,0,0};
constexpr uint8_t kStartPrefix[] = {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,
    0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x81,0x90,0xe1,0x0c,0};
constexpr uint8_t kStopPrefix[] = {0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x83,0xec,0x20,
    0x48,0x8b,0xf9,0x48,0x89,0x6c,0x24,0x30,0x48,0x81,0xc1,0xa0,0xe1,0x0c,0};
constexpr uint8_t kShutdownPrefix[] = {0x48,0x89,0x5c,0x24,8,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,
    0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x20,0x80,0xb9,0x31,3,0,0,0};
template<size_t N> U find_prefix(const uint8_t (&expected)[N])
{
    const U base = game_base();
    if (!base) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    U found = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const size_t size = section.Misc.VirtualSize;
        if (size < N || section.VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
            size > nt->OptionalHeader.SizeOfImage - section.VirtualAddress) continue;
        const auto* bytes = reinterpret_cast<const uint8_t*>(base + section.VirtualAddress);
        for (size_t offset = 0; offset + N <= size; ++offset) {
            if (memcmp(bytes + offset, expected, N)) continue;
            if (found) return 0; // Ambiguous patterns must never select a guessed entry point.
            found = reinterpret_cast<U>(bytes + offset);
        }
    }
    return found;
}
bool install()
{
    g_mode = std::clamp(int(GetPrivateProfileIntW(L"net", L"editor_background", 0, settings_ini())), 0, 2);
    if (!g_mode) return true;
    bool audited = false;
    for (const auto build : kAuditedBuilds) audited = audited || build == game_build();
    if (!audited) { swlog("editor background: unsupported prototype build; off"); return true; }
    if (!resolve(kResolveEditor)) return false;
    g_editor_vtable = addrs().editor_vtable;
    g_editor_entry = field<U>(g_editor_vtable, 0x30);
    g_host = game_ptr<const volatile uint8_t*>(0x140ceb6ff);
    if (!g_host || !g_editor_entry) return false;
    if (g_mode == 1) { // no worker, lifecycle or editor hooks in observation mode
        g_ready = true;
        swlog("editor background: ownership audit only; native scheduling unchanged");
        return true;
    }
    const U transition = find_prefix(kTransitionPrefix), start = find_prefix(kStartPrefix);
    const U stop_entry = find_prefix(kStopPrefix), shutdown = find_prefix(kShutdownPrefix);
    if (!transition || !start || !stop_entry || !shutdown) {
        swlog("editor background: lifecycle instruction check failed; off");
        return false;
    }
    g_start = reinterpret_cast<Worker>(start);
    g_stop = reinterpret_cast<Worker>(stop_entry);
    // No active session until ALL hooks install; partial installation just forwards its original functions.
    bool ok = hook_address(g_editor_entry, reinterpret_cast<void*>(&editor_detour), reinterpret_cast<void**>(&g_editor)) &&
        hook_address(transition, reinterpret_cast<void*>(&transition_detour), reinterpret_cast<void**>(&g_transition)) &&
        hook_address(shutdown, reinterpret_cast<void*>(&shutdown_detour), reinterpret_cast<void**>(&g_shutdown)) &&
        hook_game(0x140848ae0, reinterpret_cast<void*>(&frame_detour), reinterpret_cast<void**>(&g_frame));
    g_ready = ok;
    swlog("editor background: %s (experimental; vehicle editor only, distinct preview world required)", ok ? "ready" : "FAILED, off");
    return ok;
}
HookRegistrar reg{{"editor background replay prototype", true, &install}};
} // namespace

bool editor_background_frame(uintptr_t game, uintptr_t application, uintptr_t data)
{
    if (!g_ready || *g_host || !candidate(game)) return false;
    if (g_mode == 2 && GetCurrentThreadId() != g_main_thread) return false;
    U editor = field<U>(game, kFront);
    U sim = field<U>(game, kSimulation);
    U preview_world = field<U>(editor, kEditorWorld);
    U live_world = sim ? field<U>(sim, kSimulationWorld) : 0;
    if (g_mode == 2 && g_coordinator.active(game)) {
        if (!g_coordinator.frontend(game, editor, GetCurrentThreadId()) || changing(game) || !connected(data)) {
            stop(game);
            return false;
        }
    } else {
        auto reason = editor_replay::eligibility(preview_world, live_world, field<uint8_t>(game, kAsyncAllowed) != 0,
                                                 field<uint8_t>(game, kWorkerRunning) != 0, changing(game));
        if (g_observed_editor != editor) {
            g_observed_editor = editor;
            g_log_at = GetTickCount();
            g_frames = 0;
            swlog("editor background: preview world %p, live world %p: %s; mode %d", reinterpret_cast<void*>(preview_world),
                  reinterpret_cast<void*>(live_world), refusal(reason), g_mode);
        }
        // Exact same game root means shared physics/scene/asset state. Do not turn an audit failure into a race.
        if (g_mode != 2 || reason != editor_replay::Refusal::none || !connected(data)) return false;
        if (g_failed_editor == editor) return false;
        g_worker_rounds.store(0);
        g_queue.store(0);
        if (!g_coordinator.start({game, editor, g_main_thread}, &start_worker, &stop_worker)) {
            g_failed_editor = editor;
            swlog("editor background: startup failed; inline fallback for this editor visit");
            return false;
        }
        swlog("editor background: native worker started; editor updates remain on main thread %lu", g_main_thread);
    }
    // Call through the actual entry point so other mods' editor hooks still run. Its native callback signature
    // is self, application, data, &current_state, game-to-logic queue, logic-to-game queue.
    reinterpret_cast<Editor>(g_editor_entry)(editor, application, data, game + kCurrent, game + 0xce6b0, game + 0xce5f8);
    ++g_frames;
    DWORD now = GetTickCount();
    if (now - g_log_at >= 10000) {
        swlog("editor background 10s: %.1f editor frames/s on main thread, %llu replay rounds on worker; queue %u",
              g_frames * 1000.0 / (now - g_log_at), static_cast<unsigned long long>(g_worker_rounds.exchange(0)), g_queue.load());
        g_log_at = now;
        g_frames = 0;
    }
    return true;
}

void editor_background_world_tick(uintptr_t game)
{
    if (g_ready && g_coordinator.owns(game))
        g_queue.store(field<uint32_t>(game, kQueue), std::memory_order_relaxed);
}
