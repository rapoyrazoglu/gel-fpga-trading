// include/gel/platform/account_router.hpp
//
// Step 0 of the GEL pipeline (runs before risk): assign Order.account_id
// when the strategy didn't.
//
// Hybrid mode (the user's choice):
//   - If Order.account_id is already set (≠ 0): honor it as-is. Strategy
//     wanted a specific destination — for example PCTA shorting AAPL on
//     IB_PROP because OANDA doesn't list it.
//   - If Order.account_id == 0: look up the (strategy_id, symbol_id)
//     policy table and fill it in. Default routing keeps strategies
//     simple — they can ignore accounts entirely if they want.
//
// Setup-only writes; hot path is read-only.
//
#pragma once

#include "account_registry.hpp"
#include "connector/wire.hpp"
#include <unordered_map>

namespace gel::platform {

    using connector::Order;
    using connector::AccountId;
    using connector::SymbolId;
    using connector::StrategyId;
    using connector::ACCOUNT_INVALID;

    class AccountRouter {
    public:
        // Catch-all for a strategy: if no symbol-specific rule matches.
        void set_default(StrategyId sid, AccountId aid) {
            default_per_strategy_[static_cast<uint32_t>(sid)] = aid;
        }

        // Per-(strategy, symbol) override of the default.
        void set_route(StrategyId sid, SymbolId sym, AccountId aid) {
            per_symbol_[symbol_key(static_cast<uint32_t>(sid), sym)] = aid;
        }

        // Hot path. Mutates the Order in place if account_id was unset.
        // Returns the resolved account_id (0 if no rule matched — caller
        // can route to default broker / synthesize a reject).
        AccountId resolve(Order& o) noexcept {
            if (o.account_id != ACCOUNT_INVALID) return o.account_id;

            // Per-symbol rule beats per-strategy default.
            const auto k = symbol_key(o.strategy_id, o.symbol_id);
            if (auto it = per_symbol_.find(k); it != per_symbol_.end()) {
                o.account_id = it->second;
                return o.account_id;
            }
            if (auto it2 = default_per_strategy_.find(o.strategy_id);
                it2 != default_per_strategy_.end()) {
                o.account_id = it2->second;
                return o.account_id;
            }
            return ACCOUNT_INVALID;
        }

    private:
        static uint64_t symbol_key(uint32_t sid, SymbolId sym) noexcept {
            return (static_cast<uint64_t>(sid) << 32) | sym;
        }

        std::unordered_map<uint32_t, AccountId>  default_per_strategy_;
        std::unordered_map<uint64_t, AccountId>  per_symbol_;
    };

} // namespace gel::platform
