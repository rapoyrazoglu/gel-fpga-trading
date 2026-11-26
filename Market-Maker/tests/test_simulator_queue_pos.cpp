// MMX — Acceptance test #8 (spec §10):
// Synthetic LOB: our order is at queue position 5; with 4 lots of same-side
// flow ahead, we must NOT fill.

#include <cstdint>

#include "sim/queue_model.h"
#include "test_helpers.h"

int main() {
    mmx::QueueModel q;

    // Queue position 5: we joined behind 5 resting lots.
    q.place(/*price=*/100, /*qty=*/1, /*queue_ahead=*/5);

    // 4 lots of same-side aggressive flow arrive.
    uint32_t filled = q.consume_aggressive(4);
    MMX_EXPECT_EQ(filled, 0u);
    MMX_EXPECT(q.active());
    MMX_EXPECT_EQ(q.queue_ahead(), 1u);

    // One more lot eats the last queued order; the next lot fills us.
    filled = q.consume_aggressive(1);
    MMX_EXPECT_EQ(filled, 0u);
    MMX_EXPECT_EQ(q.queue_ahead(), 0u);
    filled = q.consume_aggressive(1);
    MMX_EXPECT_EQ(filled, 1u);
    MMX_EXPECT(!q.active());

    // Cancels-ahead reduce queue_ahead, never fill us.
    q.place(/*price=*/100, /*qty=*/1, /*queue_ahead=*/3);
    q.on_ahead_cancel(2);
    MMX_EXPECT_EQ(q.queue_ahead(), 1u);
    MMX_EXPECT(q.active());
    q.on_ahead_cancel(10);
    MMX_EXPECT_EQ(q.queue_ahead(), 0u);

    MMX_OK();
}
