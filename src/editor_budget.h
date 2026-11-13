#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Admission policy only. Every selected tick still runs in the game's one native call on the
// main thread, followed by its one editor callback. No concurrent world access or extra UI passes.
//
// A native call costs a fixed part (receiving, the editor's own update, UI events) plus the ticks it
// replays. Up to 1.11.8 the whole call was divided by its ticks: with a fixed part over the budget a
// call of one tick always "overran", the estimate never fell and the mode never admitted a second
// tick (2026-11-12 audit). The fixed part is now learnt from calls that replayed nothing (the native
// formula often asks for 0 ticks while the backlog is small): it follows a lower sample at once and a
// higher one slowly, so a hidden tick in an ambiguous sample can't inflate it. Unknown = 0, which is
// the old behaviour.
//
// Real time (1.11.11): the native formula plays the ticks its clock says are due, one per 16 ms. A cap of one tick per
// editor frame lost ground on every frame slower than 16 ms, for good: a 41 fps editor replayed 41 of the host's 62.5
// ticks/s (2026-11-10 01:38, backlog 45 -> 264 in 20 s). With frame_ms given, the cap also admits the ticks the frame's
// own length asks for (ceil(frame_ms / 16)), as long as a tick costs less than 16 ms; when ticks cost more, no batch
// keeps up and the editor's frame rate comes first (one tick). The cap is only an upper bound: the native formula still
// decides how many are due. Draining a backlog over 14 ticks uses budget_ms on top, as before.
namespace editor_replay {
class Budget {
    uintptr_t context_ = 0;
    double tick_ms_ = 0;
    double fixed_ms_ = 0;
    bool fixed_known_ = false;
    bool overran_ = false;
    unsigned cooldown_ = 0;
    int previous_ = 1; // drain part of the cap (ramps up one tick per frame)
    int owed_ = 1;     // real-time part of the last cap
public:
    static constexpr int max_ticks = 4;
    static constexpr double tick_period_ms = 16;
    void reset() { context_ = 0; tick_ms_ = 0; fixed_ms_ = 0; fixed_known_ = false; overran_ = false; cooldown_ = 0; previous_ = 1; owed_ = 1; }
    double fixed_ms() const { return fixed_known_ ? fixed_ms_ : 0; }
    double tick_ms() const { return tick_ms_; }
    int owed() const { return owed_; }
    bool overran() const { return overran_; } // the last observed call's replay part exceeded the budget
    // frame_ms: time since the previous editor frame (0 = unknown: one tick of real time).
    int choose(uintptr_t context, uint32_t queued, double budget_ms, double frame_ms = 0) {
        if (context_ != context) { reset(); context_ = context; }
        owed_ = 1;
        if (cooldown_) { --cooldown_; return previous_ = 1; }
        if (!(tick_ms_ > 0)) return previous_ = 1;
        if (tick_ms_ < tick_period_ms && std::isfinite(frame_ms) && frame_ms > tick_period_ms)
            owed_ = std::min(max_ticks, int(std::ceil(std::min(frame_ms, 100.0) / tick_period_ms)));
        int drain = 1;
        if (queued > 14 && budget_ms > 0) {
            // Clamp before converting to integer, including extremely cheap synthetic ticks.
            const int allowed = int(std::clamp(budget_ms / tick_ms_, 1.0, double(max_ticks)));
            drain = std::min(allowed, previous_ + 1);
        }
        previous_ = drain;
        return std::min(max_ticks, drain + owed_ - 1);
    }
    void observed(double elapsed_ms, uint32_t before, uint32_t after, int cap, double budget_ms) {
        overran_ = false;
        // Real-time ticks cost less than 16 ms each by construction; only time beyond them counts against the budget.
        budget_ms += (owed_ - 1) * tick_period_ms;
        if (!std::isfinite(elapsed_ms) || elapsed_ms < 0) { tick_ms_ = 0; cooldown_ = 8; previous_ = 1; overran_ = true; return; }
        // Empty clock polls and arrivals hiding progress are not measurements of a physics tick.
        // Learning a cheap sample from them would over-admit the next burst.
        const unsigned removed = before > after ? before - after : 0;
        if (!removed) {
            previous_ = 1;
            if (before == after) { // nothing replayed (or exactly hidden by arrivals: filtered by the slow rise)
                if (!fixed_known_ || elapsed_ms < fixed_ms_) fixed_ms_ = elapsed_ms, fixed_known_ = true;
                else fixed_ms_ += (elapsed_ms - fixed_ms_) * 0.05;
            }
            if (elapsed_ms - fixed_ms() > budget_ms) { cooldown_ = 8; overran_ = true; }
            return;
        }
        const unsigned ticks = std::clamp(removed, 1u, unsigned(std::max(1, cap)));
        const double replay_ms = std::max(0.0, elapsed_ms - fixed_ms());
        const double sample = std::max(0.01, replay_ms / ticks);
        // React to expensive ticks immediately; recover gradually when they become cheap again.
        tick_ms_ = std::max(sample, tick_ms_ * 0.9);
        if (replay_ms > budget_ms) { cooldown_ = 8; previous_ = 1; overran_ = true; }
    }
};
} // namespace editor_replay
