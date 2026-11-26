// MMX — sim/queue_model.cpp

#include "queue_model.h"

namespace mmx {

void QueueModel::place(int32_t price, uint32_t qty, uint32_t queue_ahead) {
    active_      = true;
    price_       = price;
    qty_         = qty;
    queue_ahead_ = queue_ahead;
}

void QueueModel::cancel() {
    active_      = false;
    qty_         = 0;
    queue_ahead_ = 0;
}

uint32_t QueueModel::consume_aggressive(uint32_t qty) {
    if (!active_) return 0;
    uint32_t remaining = qty;

    // Eat the queue ahead first.
    if (queue_ahead_ > 0) {
        uint32_t eat = remaining < queue_ahead_ ? remaining : queue_ahead_;
        queue_ahead_ -= eat;
        remaining   -= eat;
        if (remaining == 0) return 0;
    }

    // Now fill us.
    uint32_t fill = remaining < qty_ ? remaining : qty_;
    qty_ -= fill;
    if (qty_ == 0) active_ = false;
    return fill;
}

void QueueModel::on_ahead_cancel(uint32_t qty) {
    if (!active_) return;
    if (qty >= queue_ahead_) queue_ahead_ = 0;
    else                     queue_ahead_ -= qty;
}

}  // namespace mmx
