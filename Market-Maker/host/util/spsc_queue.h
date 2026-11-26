// MMX — host/util/spsc_queue.h
// Bounded single-producer / single-consumer lock-free ring buffer.
// Capacity must be a power of two. Non-blocking try_push / try_pop.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mmx {

template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "Use trivially copyable payloads on the fast path");

  public:
    SpscQueue() : head_(0), tail_(0) {}

    bool try_push(const T& v) {
        const auto h = head_.load(std::memory_order_relaxed);
        if (h - tail_cache_ >= Capacity) {  // looks full per cached tail
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h - tail_cache_ >= Capacity) return false;  // really full
        }
        buf_[h & (Capacity - 1)] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        const auto t = tail_.load(std::memory_order_relaxed);
        if (head_cache_ == t) {  // looks empty per cached head
            head_cache_ = head_.load(std::memory_order_acquire);
            if (head_cache_ == t) return false;  // really empty
        }
        out = buf_[t & (Capacity - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    std::size_t size() const {
        return head_.load(std::memory_order_acquire) -
               tail_.load(std::memory_order_acquire);
    }

  private:
    // Producer-owned: head_ and a consumer-side cache of tail_.
    alignas(64) std::atomic<uint64_t> head_;
    uint64_t tail_cache_{0};  // producer-private cached copy of tail_
    // Consumer-owned: tail_ and a producer-side cache of head_.
    alignas(64) std::atomic<uint64_t> tail_;
    uint64_t head_cache_{0};  // consumer-private cached copy of head_
    alignas(64) T buf_[Capacity];
};

}  // namespace mmx
