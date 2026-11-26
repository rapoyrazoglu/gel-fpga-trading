// include/gel/platform/drop_copy.hpp
//
// Block ⑦ in deck slide 6 — per-(strategy, account) reconciliation.
//
// Every ExecReport flows through here:
//   - Updates PnL keyed on (strategy, account)
//   - Forwards to the strategy via its inbound queue (on_fill)
//   - Powers the dynamic capital allocator (rolling Sharpe per cell)
//
#pragma once

#include "connector/wire.hpp"
#include <array>
#include <atomic>
#include <functional>
#include <unordered_map>

namespace gel::platform {

    using connector::ExecReport;
    using connector::ExecStatus;
    using connector::StrategyId;
    using connector::AccountId;

    struct CellPnL {
        std::atomic<int64_t> realized_pnl_ticks{0};
        std::atomic<int64_t> filled_qty{0};
        std::atomic<int64_t> rejected_count{0};
        std::atomic<int64_t> filled_count{0};

        CellPnL() = default;
        CellPnL(const CellPnL& o) {
            realized_pnl_ticks.store(o.realized_pnl_ticks.load(std::memory_order_relaxed));
            filled_qty        .store(o.filled_qty        .load(std::memory_order_relaxed));
            rejected_count    .store(o.rejected_count    .load(std::memory_order_relaxed));
            filled_count      .store(o.filled_count      .load(std::memory_order_relaxed));
        }
    };

    // Aggregate PnL view per strategy (sum across all its accounts).
    struct StrategyPnL {
        int64_t realized_pnl_ticks = 0;
        int64_t filled_qty         = 0;
        int64_t rejected_count     = 0;
        int64_t filled_count       = 0;
    };

    class DropCopy {
    public:
        using StrategyCallback = std::function<void(const ExecReport&)>;

        void on_report(const ExecReport& r) {
            const auto key  = cell_key(r.strategy_id, r.account_id);
            auto& cell      = cells_[key];

            switch (static_cast<ExecStatus>(r.status)) {
                case ExecStatus::FILLED:
                case ExecStatus::PARTIAL: {
                    // Compute the product in 128-bit to avoid int64 overflow,
                    // then narrow for the accumulator (realized_pnl_ticks stays int64).
                    const __int128 notional =
                        static_cast<__int128>(r.fill_price_ticks) *
                        static_cast<__int128>(r.fill_quantity);
                    cell.realized_pnl_ticks.fetch_add(static_cast<int64_t>(-notional), std::memory_order_relaxed);
                    cell.filled_qty        .fetch_add(r.fill_quantity,   std::memory_order_relaxed);
                    cell.filled_count      .fetch_add(1,                 std::memory_order_relaxed);
                    break;
                }
                case ExecStatus::REJECTED:
                    cell.rejected_count.fetch_add(1, std::memory_order_relaxed);
                    break;
                default:
                    break;
            }

            // Forward to the strategy's inbound queue.
            const auto sid = r.strategy_id & STRAT_MASK;
            if (sid < callbacks_.size()) {
                if (auto& cb = callbacks_[sid]) cb(r);
            }
        }

        void set_strategy_callback(StrategyId sid, StrategyCallback cb) {
            callbacks_[static_cast<uint32_t>(sid) & STRAT_MASK] = std::move(cb);
        }

        // Per-cell PnL — used by the dynamic capital allocator.
        const CellPnL& cell(StrategyId sid, AccountId aid) const {
            auto it = cells_.find(cell_key(static_cast<uint32_t>(sid), aid));
            return it == cells_.end() ? empty_ : it->second;
        }

        // Aggregate across accounts — strategy-level scoreboard.
        StrategyPnL pnl(StrategyId sid) const {
            StrategyPnL out{};
            const uint32_t s = static_cast<uint32_t>(sid);
            for (const auto& [k, c] : cells_) {
                if ((k >> 32) != s) continue;
                out.realized_pnl_ticks += c.realized_pnl_ticks.load(std::memory_order_relaxed);
                out.filled_qty         += c.filled_qty        .load(std::memory_order_relaxed);
                out.rejected_count     += c.rejected_count    .load(std::memory_order_relaxed);
                out.filled_count       += c.filled_count      .load(std::memory_order_relaxed);
            }
            return out;
        }

        template <typename F>
        void for_each_cell(F&& f) const {
            for (const auto& [k, c] : cells_) {
                f(static_cast<StrategyId>(k >> 32),
                  static_cast<AccountId>(k & 0xFFFFFFFF),
                  c);
            }
        }

    private:
        static constexpr uint32_t STRAT_MASK = 31;

        static uint64_t cell_key(uint32_t sid, AccountId aid) noexcept {
            return (static_cast<uint64_t>(sid) << 32) | aid;
        }

        std::unordered_map<uint64_t, CellPnL>          cells_;
        std::array<StrategyCallback, 32>               callbacks_{};
        CellPnL                                        empty_{};
    };

} // namespace gel::platform
