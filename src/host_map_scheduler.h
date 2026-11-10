#pragma once
#include <algorithm>
#include <cmath>

// Wall-clock credit, not simulated time. Bound recovery after a pause so a map
// frame cannot turn a multi-second stall into an unlimited replay burst.
namespace host_map {
inline constexpr int default_budget_ms = 32;
class Scheduler {
    double previous_ = -1, credit_ms_ = 0, period_ = 16;
public:
    static constexpr int max_ticks = 4;
    void reset() { previous_ = -1; credit_ms_ = 0; }
    void frame(double now, double period) {
        period_ = std::clamp(period, 1.0, 1000.0);
        if (previous_ < 0 || now < previous_) credit_ms_ = period_;
        else credit_ms_ += std::min(now - previous_, 250.0);
        previous_ = now;
        credit_ms_ = std::min(credit_ms_, max_ticks * period_);
    }
    bool due() const { return credit_ms_ + 1e-6 >= period_; }
    void consumed() { credit_ms_ = std::max(0.0, credit_ms_ - period_); }
};
}
