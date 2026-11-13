#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

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
