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
namespace editor_replay {
class Budget {
    uintptr_t context_ = 0;
    double tick_ms_ = 0;
    double fixed_ms_ = 0;
    bool fixed_known_ = false;
    bool overran_ = false;
    unsigned cooldown_ = 0;
    int previous_ = 1;
public:
    static constexpr int max_ticks = 4;
    void reset() { context_ = 0; tick_ms_ = 0; fixed_ms_ = 0; fixed_known_ = false; overran_ = false; cooldown_ = 0; previous_ = 1; }
    double fixed_ms() const { return fixed_known_ ? fixed_ms_ : 0; }
    bool overran() const { return overran_; } // the last observed call's replay part exceeded the budget
    int choose(uintptr_t context, uint32_t queued, double budget_ms) {
        if (context_ != context) { reset(); context_ = context; }
        if (queued <= 14 || !(budget_ms > 0)) return previous_ = 1;
        if (cooldown_) { --cooldown_; return previous_ = 1; }
        if (!(tick_ms_ > 0)) return previous_ = 1;
        // Clamp before converting to integer, including extremely cheap synthetic ticks.
        const int allowed = int(std::clamp(budget_ms / tick_ms_, 1.0, double(max_ticks)));
        return previous_ = std::min(allowed, previous_ + 1);
    }
    void observed(double elapsed_ms, uint32_t before, uint32_t after, int cap, double budget_ms) {
        overran_ = false;
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
