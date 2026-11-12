#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Admission policy only. Every selected tick still runs in the game's one native call on the
// main thread, followed by its one editor callback. No concurrent world access or extra UI passes.
namespace editor_replay {
class Budget {
    uintptr_t context_ = 0;
    double tick_ms_ = 0;
    unsigned cooldown_ = 0;
    int previous_ = 1;
public:
    static constexpr int max_ticks = 4;
    void reset() { context_ = 0; tick_ms_ = 0; cooldown_ = 0; previous_ = 1; }
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
        if (!std::isfinite(elapsed_ms) || elapsed_ms < 0) { tick_ms_ = 0; cooldown_ = 8; previous_ = 1; return; }
        // Empty clock polls and arrivals hiding progress are not measurements of a physics tick.
        // Learning a cheap sample from them would over-admit the next burst.
        const unsigned removed = before > after ? before - after : 0;
        if (!removed) {
            previous_ = 1;
            if (elapsed_ms > budget_ms) cooldown_ = 8;
            return;
        }
        const unsigned ticks = std::clamp(removed, 1u, unsigned(std::max(1, cap)));
        const double sample = std::max(0.01, elapsed_ms / ticks);
        // React to expensive ticks immediately; recover gradually when they become cheap again.
        tick_ms_ = std::max(sample, tick_ms_ * 0.9);
        if (elapsed_ms > budget_ms) { cooldown_ = 8; previous_ = 1; }
    }
};
} // namespace editor_replay
