// connector/shm_ring.hpp
//
// Cross-process SPSC ring backed by a mmap'd file.
//
// One process creates the ring file (POSIX shm or a regular file under
// /tmp); another attaches to it. Both see the same memory. The producer
// writes to head_, the consumer advances tail_, exactly like the
// in-process ring.hpp.
//
// Constraints (same as ring.hpp):
//   - One producer, one consumer. No multi-producer / multi-consumer.
//   - T must be trivially copyable + standard layout (it lives in a file).
//   - Capacity must be a power of two.
//   - Cacheline padding so head/tail don't false-share.
//
// File layout (all little-endian, native struct layout):
//
//   offset 0      head        (uint64 atomic, padded to 64 B)
//   offset 64     tail        (uint64 atomic, padded to 64 B)
//   offset 128    buffer[Capacity * sizeof(T)]
//
// IMPORTANT: producer and consumer must agree on T and Capacity. Mismatch
// = silent corruption. The wire types in connector/wire.hpp are versioned
// implicitly through their static_assert sizes — if a wire struct grows,
// both sides must rebuild.
//
#pragma once

#include "ring.hpp"   // DEFAULT_ORDER_RING_CAP / DEFAULT_EVENT_RING_CAP
#include "wire.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>

namespace connector {

    template <typename T, std::size_t Capacity>
    class ShmRing {
        static_assert((Capacity & (Capacity - 1)) == 0,
                      "Capacity must be a power of two");
        static_assert(Capacity >= 2, "Capacity must be >= 2");
        static_assert(std::is_trivially_copyable<T>::value,
                      "ShmRing requires trivially copyable payload");
        static_assert(std::is_standard_layout<T>::value,
                      "ShmRing requires standard-layout payload (file-mapped)");
        // The indices live in a file shared by two processes: a locking
        // atomic would keep its lock in process memory and protect nothing.
        static_assert(std::atomic<uint64_t>::is_always_lock_free,
                      "ShmRing indices must be lock-free atomics");

        struct Header {
            alignas(64) std::atomic<uint64_t> head;
            alignas(64) std::atomic<uint64_t> tail;
            alignas(64) T                     buffer[Capacity];
        };
        static_assert(std::is_standard_layout<Header>::value,
                      "Header must be standard-layout to safely mmap");

    public:
        // create == true  → ftruncate the file to the right size and zero
        //                   the header (use this on the engine/server side).
        // create == false → attach to an existing file (strategy/client side).
        ShmRing(const std::string& path, bool create) {
            const size_t bytes = sizeof(Header);
            // Owner-only, and never through a symlink: ring files often live in
            // a shared directory like /tmp.
            const int flags    = O_RDWR | O_NOFOLLOW | (create ? O_CREAT : 0);
            fd_ = ::open(path.c_str(), flags, 0600);
            if (fd_ < 0) {
                throw std::runtime_error("shm open failed for '" + path + "': " +
                                         std::strerror(errno));
            }
            // O_CREAT reuses an existing file, so a file somebody else planted
            // at this path would be mapped as-is. Accept only a regular file
            // that we own, that nobody else can open, and that has no second
            // name (a hard link to one of our own private files would pass the
            // owner check and then be truncated and overwritten).
            struct stat own;
            if (::fstat(fd_, &own) != 0 || !S_ISREG(own.st_mode) ||
                own.st_uid != ::geteuid() || (own.st_mode & 077) != 0 ||
                own.st_nlink != 1) {
                ::close(fd_);
                throw std::runtime_error(
                    "shm file '" + path + "' is not a private regular file owned "
                    "by this user (rings from older builds: delete and recreate)");
            }
            if (create) {
                if (::ftruncate(fd_, static_cast<off_t>(bytes)) != 0) {
                    const int e = errno;
                    ::close(fd_);
                    throw std::runtime_error(std::string("shm ftruncate failed: ") +
                                             std::strerror(e));
                }
            } else {
                struct stat st;
                if (::fstat(fd_, &st) != 0 || static_cast<size_t>(st.st_size) < bytes) {
                    ::close(fd_);
                    throw std::runtime_error("shm file too small or stat failed: " + path);
                }
            }
            void* addr = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd_, 0);
            if (addr == MAP_FAILED) {
                const int e = errno;
                ::close(fd_);
                throw std::runtime_error(std::string("shm mmap failed: ") +
                                         std::strerror(e));
            }
            hdr_ = static_cast<Header*>(addr);
            if (create) {
                hdr_->head.store(0, std::memory_order_relaxed);
                hdr_->tail.store(0, std::memory_order_relaxed);
            }
        }

        ~ShmRing() noexcept {
            if (hdr_) ::munmap(hdr_, sizeof(Header));
            if (fd_ >= 0) ::close(fd_);
        }

        ShmRing(const ShmRing&)            = delete;
        ShmRing& operator=(const ShmRing&) = delete;

        bool try_push(const T& item) noexcept {
            // Indices live in memory the peer can write: mask every load so a
            // corrupt value can never index outside the buffer.
            const auto h    = hdr_->head.load(std::memory_order_relaxed) & MASK;
            const auto next = (h + 1) & MASK;
            if (next == (hdr_->tail.load(std::memory_order_acquire) & MASK)) {
                return false; // full
            }
            hdr_->buffer[h] = item;
            hdr_->head.store(next, std::memory_order_release);
            return true;
        }

        bool try_pop(T& out) noexcept {
            const auto t = hdr_->tail.load(std::memory_order_relaxed) & MASK;
            if (t == (hdr_->head.load(std::memory_order_acquire) & MASK)) {
                return false; // empty
            }
            out = hdr_->buffer[t];
            hdr_->tail.store((t + 1) & MASK, std::memory_order_release);
            return true;
        }

        std::size_t size_approx() const noexcept {
            const auto h = hdr_->head.load(std::memory_order_acquire);
            const auto t = hdr_->tail.load(std::memory_order_acquire);
            return static_cast<std::size_t>((h - t) & MASK);
        }

        static constexpr std::size_t capacity() noexcept { return Capacity - 1; }

    private:
        static constexpr uint64_t MASK = Capacity - 1;

        int     fd_  = -1;
        Header* hdr_ = nullptr;
    };

    // Default cross-process ring sizes. Same as in-process defaults so a
    // strategy can switch transports without changing its risk model.
    using OrderShmRing  = ShmRing<Order,      DEFAULT_ORDER_RING_CAP>;
    using ReportShmRing = ShmRing<ExecReport, DEFAULT_EVENT_RING_CAP>;

} // namespace connector
