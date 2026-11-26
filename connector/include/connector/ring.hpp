// connector/ring.hpp
//
// Single-Producer Single-Consumer ring buffer (in-process).
//
// This is the "wire" between a strategy and the engine when both run in
// the same process. Each strategy gets two:
//   - outbound: strategy writes Order, engine reads
//   - inbound:  engine writes ExecReport / Tick / Event, strategy reads
//
// Properties:
//   - Lock-free, wait-free, no allocations after construction.
//   - Cacheline padding between head and tail to avoid false sharing.
//   - Power-of-two capacity → mask instead of modulo.
//   - Acquire/release fences only — no seq_cst.
//   - Trivially copyable T → bytewise copy is safe (memcpy-equivalent).
//
// For cross-process use, see shm_ring.hpp — same algorithm, mmap-backed.
//
#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace connector {

    inline constexpr std::size_t CACHELINE_BYTES = 64;

    template <typename T, std::size_t Capacity>
    class SpscRing {
        static_assert((Capacity & (Capacity - 1)) == 0,
                      "Capacity must be a power of two");
        static_assert(Capacity >= 2, "Capacity must be >= 2");
        static_assert(std::is_trivially_copyable_v<T>,
                      "SpscRing requires trivially copyable payload");
        // A locking atomic index would put a mutex on the hot path.
        static_assert(std::atomic<std::size_t>::is_always_lock_free,
                      "SpscRing indices must be lock-free atomics");

    public:
        SpscRing() = default;

        SpscRing(const SpscRing&)            = delete;
        SpscRing& operator=(const SpscRing&) = delete;

        bool try_push(const T& item) noexcept {
            const auto head = head_.load(std::memory_order_relaxed);
            const auto next = (head + 1) & MASK;
            if (next == tail_.load(std::memory_order_acquire)) {
                return false; // full
            }
            buffer_[head] = item;
            head_.store(next, std::memory_order_release);
            return true;
        }

        bool try_pop(T& out) noexcept {
            const auto tail = tail_.load(std::memory_order_relaxed);
            if (tail == head_.load(std::memory_order_acquire)) {
                return false; // empty
            }
            out = buffer_[tail];
            tail_.store((tail + 1) & MASK, std::memory_order_release);
            return true;
        }

        [[nodiscard]] std::size_t size_approx() const noexcept {
            const auto h = head_.load(std::memory_order_acquire);
            const auto t = tail_.load(std::memory_order_acquire);
            return (h - t) & MASK;
        }

        [[nodiscard]] static constexpr std::size_t capacity() noexcept {
            return Capacity - 1; // one slot reserved as full/empty discriminator
        }

    private:
        static constexpr std::size_t MASK = Capacity - 1;

        // head_ and tail_ go on separate cachelines so the producer and
        // consumer don't ping-pong each other's L1.
        alignas(CACHELINE_BYTES) std::atomic<std::size_t> head_{0};
        alignas(CACHELINE_BYTES) std::atomic<std::size_t> tail_{0};
        alignas(CACHELINE_BYTES) std::array<T, Capacity>  buffer_{};
    };

    // Default capacities:
    //   64K orders × 64 B = 4 MB per strategy outbound ring.
    //   16K events × 64 B = 1 MB per strategy inbound ring.
    inline constexpr std::size_t DEFAULT_ORDER_RING_CAP = 65'536;
    inline constexpr std::size_t DEFAULT_EVENT_RING_CAP = 16'384;

} // namespace connector
