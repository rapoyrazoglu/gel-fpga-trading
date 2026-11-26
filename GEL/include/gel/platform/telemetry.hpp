// include/gel/platform/telemetry.hpp
//
// Per-hop timestamps so we can tear apart the 5 µs CPU budget (deck slide 7).
// In production this drains into Prometheus; here it stays in-memory and
// exposes a percentile getter for tests / the demo's final summary.
//
#pragma once

#include "connector/wire.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <vector>

namespace gel::platform {

    using connector::Hop;
    using connector::TelemetryRecord;

    inline uint64_t now_ns() noexcept {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(
                   steady_clock::now().time_since_epoch()).count();
    }

    class Telemetry {
    public:
        // Hot path: stamp a hop. Thread-safe per-hop slot.
        void stamp(uint64_t client_order_id, uint32_t strategy_id, Hop hop) noexcept {
            TelemetryRecord r{};
            r.ts_ns           = now_ns();
            r.client_order_id = client_order_id;
            r.strategy_id     = strategy_id;
            r.hop             = static_cast<uint8_t>(hop);

            // Lock-free append into the per-hop vector. Real implementation
            // would push into a ring buffer; for analysis we just collect.
            const auto idx = static_cast<size_t>(hop);
            if (idx < buckets_.size()) {
                auto slot = buckets_[idx].next.fetch_add(1, std::memory_order_relaxed);
                if (slot < BUCKET_CAP) {
                    buckets_[idx].records[slot] = r;
                }
            }
        }

        // Percentile of (later_hop - earlier_hop) latency, in ns, matched
        // by client_order_id. Costs O(N log N); call from setup/teardown.
        [[nodiscard]] uint64_t percentile_ns(Hop earlier, Hop later, double q) const {
            const auto& a = buckets_[static_cast<size_t>(earlier)];
            const auto& b = buckets_[static_cast<size_t>(later)];
            std::vector<int64_t> diffs;
            const auto an = std::min(a.next.load(), BUCKET_CAP);
            const auto bn = std::min(b.next.load(), BUCKET_CAP);
            for (size_t i = 0; i < an; ++i) {
                const auto& ra = a.records[i];
                for (size_t j = 0; j < bn; ++j) {
                    const auto& rb = b.records[j];
                    if (ra.client_order_id == rb.client_order_id) {
                        diffs.push_back(int64_t(rb.ts_ns) - int64_t(ra.ts_ns));
                        break;
                    }
                }
            }
            if (diffs.empty()) return 0;
            std::sort(diffs.begin(), diffs.end());
            const auto idx = std::min<size_t>(
                static_cast<size_t>(q * diffs.size()), diffs.size() - 1);
            return diffs[idx] < 0 ? 0 : static_cast<uint64_t>(diffs[idx]);
        }

    private:
        static constexpr size_t BUCKET_CAP = 65'536;
        struct Bucket {
            std::atomic<size_t>                  next{0};
            std::array<TelemetryRecord, BUCKET_CAP> records{};
        };
        // 8 hops mirrors enum class Hop in wire.hpp.
        std::array<Bucket, 8> buckets_;
    };

} // namespace gel::platform
