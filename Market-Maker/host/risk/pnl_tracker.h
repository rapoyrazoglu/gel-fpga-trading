// MMX — host/risk/pnl_tracker.h
// Realised + unrealised PnL bookkeeping from a stream of fills.

#pragma once

#include <cstdint>
#include <deque>

namespace mmx {

struct Fill {
    std::uint64_t ts_ns;
    int8_t        side;       // +1 = buy (we got hit on bid), -1 = sell
    int32_t       price_ticks;
    uint32_t      qty;
    int32_t       fee_ticks;  // signed: positive = we pay, negative = rebate
};

class PnlTracker {
  public:
    explicit PnlTracker(double tick_value = 1.0)
        : tick_value_(tick_value), inventory_(0), avg_cost_ticks_(0.0),
          realised_pnl_ticks_(0.0) {}

    void on_fill(const Fill& f);

    // Mark-to-market PnL using a reference mid (in ticks).
    double total_pnl(int32_t mid_ticks) const;
    double realised_pnl_ticks() const { return realised_pnl_ticks_; }
    int32_t inventory() const { return inventory_; }

    // Rolling-window PnL — caller passes a horizon in ns; tracker keeps a
    // deque of (ts, pnl_at_that_time).
    void snapshot(std::uint64_t ts_ns, int32_t mid_ticks);
    double rolling_pnl(std::uint64_t ts_ns, std::uint64_t window_ns,
                       int32_t mid_ticks) const;

  private:
    double  tick_value_;
    int32_t inventory_;
    double  avg_cost_ticks_;
    double  realised_pnl_ticks_;

    struct Snap { std::uint64_t ts_ns; double pnl; };
    std::deque<Snap> snaps_;
};

}  // namespace mmx
