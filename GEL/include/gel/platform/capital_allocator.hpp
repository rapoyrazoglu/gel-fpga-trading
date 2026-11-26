// include/gel/platform/capital_allocator.hpp
//
// Dynamic capital allocator — moves position caps between (strategy, account)
// cells based on rolling risk-adjusted PnL.
//
// The cell that has been making money lately gets a bigger cap; the cell
// that's been losing gets shrunk. Implementation: every `interval_secs`,
// snapshot DropCopy::cell PnLs, compute a rolling Sharpe, and rebalance.
//
// Tournament-style:
//
//     score(cell) = Sharpe(rolling PnL window)
//     normalized   = softmax(score / temperature)
//     new_cap(cell) = min_cap + normalized × (total_cap_pool - N × min_cap)
//
// `temperature` controls how aggressive reallocation is. Floor of `min_cap`
// per cell so a temporarily losing strategy isn't starved completely.
//
// Idempotent: safe to call from a single periodic thread; never blocks
// the order hot path.
//
#pragma once

#include "account_registry.hpp"
#include "drop_copy.hpp"
#include "risk_engine.hpp"
#include <cmath>
#include <chrono>
#include <deque>
#include <unordered_map>
#include <vector>

namespace gel::platform {

    struct CapitalAllocatorConfig {
        // How often to rebalance (wall-clock).
        int     interval_secs       = 60;
        // Total position cap pooled across all (strategy, account) cells.
        int64_t total_cap_pool      = 10'000'000;
        // Floor — each cell always gets at least this much.
        int64_t min_cap_per_cell    = 1'000;
        // Softmax temperature. Smaller = more aggressive reallocation.
        double  temperature         = 1.0;
        // Rolling window size (number of snapshots). Sharpe = mean / stdev
        // computed over the last N PnL deltas.
        size_t  rolling_window      = 30;
    };

    class CapitalAllocator {
    public:
        CapitalAllocator(RiskEngine& risk,
                         DropCopy&   drops,
                         AccountRegistry& /*accounts*/,
                         CapitalAllocatorConfig cfg = {})
            : risk_(risk), drops_(drops), cfg_(cfg) {}

        // Call this once per loop tick; it self-rate-limits to `interval_secs`.
        void maybe_rebalance() {
            using namespace std::chrono;
            const auto now = steady_clock::now();
            if (last_run_ != steady_clock::time_point{} &&
                duration_cast<seconds>(now - last_run_).count() < cfg_.interval_secs) {
                return;
            }
            last_run_ = now;
            rebalance();
        }

        // Force a rebalance regardless of the cooldown. Used by tests.
        void rebalance() {
            // 1. Snapshot every cell's current realized PnL.
            std::vector<CellSnapshot> snaps;
            drops_.for_each_cell([&](StrategyId s, AccountId a, const CellPnL& c) {
                snaps.push_back({s, a, c.realized_pnl_ticks.load(std::memory_order_relaxed)});
            });
            if (snaps.empty()) return;

            // 2. Compute rolling Sharpe per cell.
            std::vector<double> scores(snaps.size(), 0.0);
            for (size_t i = 0; i < snaps.size(); ++i) {
                const auto key  = cell_key(snaps[i].sid, snaps[i].aid);
                auto& hist      = history_[key];
                if (!hist.empty()) {
                    const int64_t prev = hist.back();
                    deltas_[key].push_back(snaps[i].pnl - prev);
                    if (deltas_[key].size() > cfg_.rolling_window) deltas_[key].pop_front();
                }
                hist.push_back(snaps[i].pnl);
                if (hist.size() > cfg_.rolling_window) hist.pop_front();
                scores[i] = sharpe(deltas_[key]);
            }

            // 3. Softmax over scores → weights.
            const auto weights = softmax(scores, cfg_.temperature);

            // 4. Allocate caps proportionally with floor.
            const int64_t pool_after_floors =
                cfg_.total_cap_pool -
                static_cast<int64_t>(snaps.size()) * cfg_.min_cap_per_cell;
            const int64_t pool_remaining = std::max<int64_t>(pool_after_floors, 0);
            for (size_t i = 0; i < snaps.size(); ++i) {
                const int64_t new_cap = cfg_.min_cap_per_cell +
                    static_cast<int64_t>(weights[i] * static_cast<double>(pool_remaining));
                risk_.update_max_position(snaps[i].sid, snaps[i].aid, new_cap);
            }
        }

    private:
        struct CellSnapshot {
            StrategyId sid;
            AccountId  aid;
            int64_t    pnl;
        };

        static uint64_t cell_key(StrategyId s, AccountId a) noexcept {
            return (static_cast<uint64_t>(static_cast<uint32_t>(s)) << 32) | a;
        }

        static double sharpe(const std::deque<int64_t>& deltas) {
            if (deltas.size() < 2) return 0.0;
            double mean = 0.0;
            for (auto d : deltas) mean += static_cast<double>(d);
            mean /= deltas.size();
            double var = 0.0;
            for (auto d : deltas) {
                const double diff = static_cast<double>(d) - mean;
                var += diff * diff;
            }
            var /= (deltas.size() - 1);
            const double sd = std::sqrt(var);
            return sd > 0.0 ? mean / sd : 0.0;
        }

        static std::vector<double> softmax(const std::vector<double>& xs, double temp) {
            if (xs.empty()) return {};
            // Clamp non-positive temperatures to a small positive value so the
            // division below can't yield inf/nan out of exp().
            if (!(temp > 0.0)) temp = 1e-3;
            double mx = xs[0];
            for (double x : xs) if (x > mx) mx = x;
            std::vector<double> out(xs.size());
            double sum = 0.0;
            for (size_t i = 0; i < xs.size(); ++i) {
                out[i] = std::exp((xs[i] - mx) / temp);
                sum += out[i];
            }
            if (sum > 0.0) for (double& v : out) v /= sum;
            else for (double& v : out) v = 1.0 / static_cast<double>(out.size());
            return out;
        }

        RiskEngine&                              risk_;
        DropCopy&                                drops_;
        CapitalAllocatorConfig                   cfg_;
        std::chrono::steady_clock::time_point    last_run_{};
        std::unordered_map<uint64_t, std::deque<int64_t>> history_;
        std::unordered_map<uint64_t, std::deque<int64_t>> deltas_;
    };

} // namespace gel::platform
