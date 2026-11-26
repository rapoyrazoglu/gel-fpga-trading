// MMX — sim/queue_model.h
// Single-level queue-position fill model (spec §9.1).
//
// Each side (bid/ask) maintains at most one outstanding resting order. We
// track queue_ahead = volume that joined the level before us. Aggressive flow
// on our side first consumes queue_ahead, then fills us. Cancels ahead of us
// reduce queue_ahead but never fill us.

#pragma once

#include <cstdint>

namespace mmx {

class QueueModel {
  public:
    QueueModel() = default;

    // Join the level at `price` behind `queue_ahead` lots already resting.
    void place(int32_t price, uint32_t qty, uint32_t queue_ahead);
    void cancel();

    bool      active()       const { return active_; }
    int32_t   price()        const { return price_; }
    uint32_t  qty_remaining()const { return qty_; }
    uint32_t  queue_ahead()  const { return queue_ahead_; }

    // Aggressive flow on OUR side for `qty` lots arrives. Returns the number
    // of lots that filled US (after the queue ahead is consumed).
    uint32_t consume_aggressive(uint32_t qty);

    // Same-side cancels for `qty` lots ahead of us reduce queue_ahead.
    void on_ahead_cancel(uint32_t qty);

  private:
    bool     active_      = false;
    int32_t  price_       = 0;
    uint32_t qty_         = 0;
    uint32_t queue_ahead_ = 0;
};

}  // namespace mmx
