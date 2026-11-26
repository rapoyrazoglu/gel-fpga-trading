// include/gel/platform/risk_engine.hpp
//
// Block ③ in deck slide 6 — pre-trade risk.
//
// Per-(strategy, account) budget:
//   - max position (long + short, per symbol)
//   - fat-finger price band (% from last mid)
//   - rate throttle (orders / second)
//   - kill-switch — TWO of them: per-strategy and per-account. Either
//     trips and the order is rejected.
//
// All checks are O(1) on the hot path. State is per-(strategy, account)
// so a bad fill on one account never affects the others.
//
#pragma once

#include "connector/wire.hpp"
#include "symbol_table.hpp"
#include "account_registry.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <unordered_map>

namespace gel::platform {

    using connector::Order;
    using connector::ExecReport;
    using connector::RejectReason;
    using connector::SymbolId;
    using connector::StrategyId;
    using connector::AccountId;
    using connector::ACCOUNT_INVALID;

    struct RiskLimits {
        int64_t  max_abs_position    = 1'000'000;  // |signed sum| cap, per symbol
        int64_t  max_order_qty       = 100'000;    // single-shot fat-finger
        double   fat_finger_band_pct = 0.05;       // |order_px - last_px| / last_px
        int      max_orders_per_sec  = 10'000;     // throttle
    };

    class RiskEngine {
    public:
        RiskEngine(const SymbolTable& symbols, AccountRegistry& accounts)
            : symbols_(symbols), accounts_(accounts) {}

        // Configure once per (strategy, account) at setup. If you skip the
        // account_id, the limits apply as a default (used by the legacy
        // single-account demo).
        void set_limits(StrategyId sid, const RiskLimits& limits) {
            default_limits_[static_cast<uint32_t>(sid) & STRAT_MASK] = limits;
        }
        void set_limits(StrategyId sid, AccountId aid, const RiskLimits& limits) {
            limits_per_account_[limit_key(static_cast<uint32_t>(sid), aid)] = limits;
        }

        void set_last_price(SymbolId sid, int64_t price_ticks) noexcept {
            if (sid < last_price_.size()) {
                last_price_[sid].store(price_ticks, std::memory_order_relaxed);
            }
        }

        // Hot path. Returns NONE on pass; populates a reason on reject.
        RejectReason check(const Order& o) noexcept {
            // 1. Symbol resolves
            const auto* meta = symbols_.lookup(o.symbol_id);
            if (!meta) return RejectReason::UNKNOWN_SYMBOL;

            // 2a. Strategy-level kill switch
            const auto& strat_kill = kill_switch_[o.strategy_id & STRAT_MASK];
            if (strat_kill.load(std::memory_order_acquire)) {
                return RejectReason::KILL_SWITCH;
            }

            // 2b. Account-level kill switch (a busted account doesn't
            //     affect the same strategy on other accounts).
            if (auto* acc = accounts_.get(o.account_id);
                acc && acc->kill_switch.load(std::memory_order_acquire)) {
                return RejectReason::KILL_SWITCH;
            }

            // Resolve limits: per-(strategy, account) overrides default.
            const RiskLimits& lim = resolve_limits(
                static_cast<StrategyId>(o.strategy_id), o.account_id);

            // 3. Quantity sanity
            if (o.quantity == 0) return RejectReason::BAD_QTY;
            const int64_t abs_qty = o.quantity < 0 ? -o.quantity : o.quantity;
            if (abs_qty > lim.max_order_qty) return RejectReason::FAT_FINGER;

            // 4. Price sanity (skip for MARKET orders; they carry -1)
            if (o.price_ticks > 0 && o.symbol_id < last_price_.size()) {
                const auto last = last_price_[o.symbol_id].load(std::memory_order_relaxed);
                if (last > 0) {
                    const auto diff = o.price_ticks > last
                                          ? o.price_ticks - last
                                          : last - o.price_ticks;
                    if (static_cast<double>(diff) / static_cast<double>(last) > lim.fat_finger_band_pct) {
                        return RejectReason::FAT_FINGER;
                    }
                }
            }

            // 5. Position check — signed net per (strategy, account, symbol)
            const auto key = position_key(o.strategy_id, o.account_id, o.symbol_id);
            auto& pos = positions_[key];
            // Guard the signed add against int64 overflow before committing.
            // An overflowing prospective position is, by definition, past any
            // sane cap — treat it as a position-limit breach (same reject path).
            if ((o.quantity > 0 && pos > INT64_MAX - o.quantity) ||
                (o.quantity < 0 && pos < INT64_MIN - o.quantity)) {
                return RejectReason::POSITION_LIMIT;
            }
            const int64_t prospective = pos + o.quantity;
            const int64_t abs_prospective = prospective < 0 ? -prospective : prospective;
            if (abs_prospective > lim.max_abs_position) {
                return RejectReason::POSITION_LIMIT;
            }

            // 6. Throttle — per (strategy, account)
            using namespace std::chrono;
            const auto now_sec = duration_cast<seconds>(
                                     steady_clock::now().time_since_epoch())
                                     .count();
            auto& bucket = throttle_[throttle_key(o.strategy_id, o.account_id)];
            if (bucket.last_sec != now_sec) {
                bucket.last_sec = now_sec;
                bucket.count    = 0;
            }
            if (++bucket.count > static_cast<uint32_t>(lim.max_orders_per_sec)) {
                return RejectReason::THROTTLE;
            }

            // Pass. Optimistically commit the position; back out on cancel/reject.
            pos = prospective;
            return RejectReason::NONE;
        }

        // Update position from drop-copy (negate cancels, fills, etc.)
        void on_fill(const ExecReport& r) noexcept {
            if (r.status == static_cast<uint8_t>(connector::ExecStatus::REJECTED) ||
                r.status == static_cast<uint8_t>(connector::ExecStatus::CANCELED)) {
                const auto key = position_key(r.strategy_id, r.account_id, r.symbol_id);
                positions_[key] -= r.fill_quantity;
            }
        }

        // Trip the kill switch for a strategy.
        void kill(StrategyId sid) noexcept {
            kill_switch_[static_cast<uint32_t>(sid) & STRAT_MASK]
                .store(true, std::memory_order_release);
        }

        void resume(StrategyId sid) noexcept {
            kill_switch_[static_cast<uint32_t>(sid) & STRAT_MASK]
                .store(false, std::memory_order_release);
        }

        [[nodiscard]] int64_t position(StrategyId s, AccountId a, SymbolId sym) const {
            auto it = positions_.find(position_key(static_cast<uint32_t>(s), a, sym));
            return it == positions_.end() ? 0 : it->second;
        }

        // Mutate the per-(strategy, account) position cap from the dynamic
        // capital allocator. Atomic via the limits map only; concurrent
        // updates aren't supported (allocator runs on a single thread).
        void update_max_position(StrategyId s, AccountId a, int64_t new_cap) {
            const auto key = limit_key(static_cast<uint32_t>(s), a);
            auto it = limits_per_account_.find(key);
            if (it == limits_per_account_.end()) {
                // Seed from the strategy default.
                limits_per_account_[key] = default_limits_[static_cast<uint32_t>(s) & STRAT_MASK];
                it = limits_per_account_.find(key);
            }
            it->second.max_abs_position = new_cap;
        }

    private:
        static constexpr uint32_t STRAT_MASK     = 31;   // 32 strategy slots
        static constexpr size_t   MAX_SYMBOLS    = 4096; // mirrors SymbolTable

        struct ThrottleBucket {
            int64_t  last_sec = 0;
            uint32_t count    = 0;
        };

        static uint64_t position_key(uint32_t strategy_id, AccountId aid, SymbolId sym) noexcept {
            // 16 bits strategy | 16 bits account | 32 bits symbol
            return (static_cast<uint64_t>(strategy_id & 0xFFFF) << 48) |
                   (static_cast<uint64_t>(aid          & 0xFFFF) << 32) |
                   sym;
        }
        static uint64_t limit_key(uint32_t strategy_id, AccountId aid) noexcept {
            return (static_cast<uint64_t>(strategy_id) << 32) | aid;
        }
        static uint32_t throttle_key(uint32_t strategy_id, AccountId aid) noexcept {
            // Pack into 5 strategy bits (mirrors STRAT_MASK) + 6 account bits
            // = 11-bit slot index, 2048 buckets total.
            return ((strategy_id & STRAT_MASK) << 6) | (aid & 0x3F);
        }

        const RiskLimits& resolve_limits(StrategyId s, AccountId a) const {
            auto it = limits_per_account_.find(
                limit_key(static_cast<uint32_t>(s), a));
            if (it != limits_per_account_.end()) return it->second;
            return default_limits_[static_cast<uint32_t>(s) & STRAT_MASK];
        }

        const SymbolTable&                            symbols_;
        AccountRegistry&                              accounts_;
        std::array<RiskLimits,    32>                 default_limits_{};
        std::unordered_map<uint64_t, RiskLimits>      limits_per_account_;
        std::array<std::atomic<bool>, 32>             kill_switch_{};
        std::array<ThrottleBucket, 2048>              throttle_{};
        std::array<std::atomic<int64_t>, MAX_SYMBOLS> last_price_{};
        std::unordered_map<uint64_t, int64_t>         positions_;
    };

} // namespace gel::platform
