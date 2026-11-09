#pragma once
#include <atomic>
#include <cstdint>

// Main thread owns start/stop/frame scheduling. Native worker callbacks only read this publication.
// Keep the session published THROUGH join: a finishing replay must not call the editor during teardown.
namespace editor_replay {
enum class Phase : uint8_t { idle, starting, running, stopping };
struct Session { uintptr_t game = 0, editor = 0; uint32_t main_thread = 0; };
using Start = bool(*)(uintptr_t);
using Stop = void(*)(uintptr_t);

enum class Refusal { none, missing_world, shared_world, async_disabled, worker_owned, transition };
inline Refusal eligibility(uintptr_t preview_world, uintptr_t live_world, bool async_enabled,
                           bool worker_running, bool changing)
{
    if (!preview_world || !live_world) return Refusal::missing_world;
    if (preview_world == live_world) return Refusal::shared_world;
    if (!async_enabled) return Refusal::async_disabled;
    if (worker_running) return Refusal::worker_owned;
    if (changing) return Refusal::transition;
    return Refusal::none;
}

class Coordinator {
    std::atomic<Phase> phase_{Phase::idle};
    // Atomic fields also make a concurrent observer safe when a joined session is replaced.
    std::atomic<uintptr_t> game_{0}, editor_{0};
    std::atomic<uint32_t> main_thread_{0};
    Stop join_ = nullptr; // main thread only
public:
    Phase phase() const { return phase_.load(std::memory_order_acquire); }
    bool owns(uintptr_t game) const { return phase() != Phase::idle && game_.load() == game; }
    bool active(uintptr_t game) const { return phase() == Phase::running && game_.load() == game; }
    bool suppress_editor(uintptr_t editor, uint32_t thread) const
    {
        return phase() != Phase::idle && editor_.load() == editor && main_thread_.load() != thread;
    }
    bool frontend(uintptr_t game, uintptr_t editor, uint32_t thread) const
    {
        return active(game) && editor_.load() == editor && main_thread_.load() == thread;
    }
    bool start(Session s, Start start_worker, Stop join_worker)
    {
        if (phase() != Phase::idle || !s.game || !s.editor || !s.main_thread || !start_worker || !join_worker)
            return false;
        game_.store(s.game);
        editor_.store(s.editor);
        main_thread_.store(s.main_thread);
        join_ = join_worker;
        phase_.store(Phase::starting, std::memory_order_release);
        // The native worker may invoke its first callback before start_worker returns.
        if (!start_worker(s.game)) {
            stop(); // even a partially started worker must be joined before unpublishing
            return false;
        }
        phase_.store(Phase::running, std::memory_order_release);
        return true;
    }
    void stop()
    {
        if (phase() == Phase::idle || phase() == Phase::stopping) return;
        phase_.store(Phase::stopping, std::memory_order_release);
        join_(game_.load());
        phase_.store(Phase::idle, std::memory_order_release);
        game_.store(0);
        editor_.store(0);
        main_thread_.store(0);
        join_ = nullptr;
    }
};
} // namespace editor_replay
