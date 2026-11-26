// connector/account_map.hpp
//
// StrategyAccountMap — N:M assignment between strategies and broker
// accounts.
//
// Use cases the engine has to support:
//   - One strategy on one account                (1:1 — basic case)
//   - Multiple strategies sharing one account    (N:1 — multi-strat book)
//   - One strategy across multiple accounts      (1:N — capital splitting,
//                                                       sub-accounts per venue)
//
// Strategies don't usually pick the account themselves; they emit orders
// with a "logical" intent and the engine stamps the right `account_id`
// using this map. A strategy that genuinely needs to fan out across
// accounts can ask the map for its account list and emit one order per
// account.
//
// Lookup is O(1) for the common (strategy → primary_account) path. Reverse
// (account → strategies) is O(num_strategies); good enough for setup-time
// queries and rare reconciliation work.
//
// Header-only, std::unordered_map / std::vector — not in any hot path.
//
#pragma once

#include "wire.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace connector {

    // -------------------------------------------------------------------
    // Per-account static info. The engine owns the actual broker handle;
    // this struct is just enough metadata for routing decisions and audit.
    // -------------------------------------------------------------------
    struct AccountInfo {
        AccountId   id           = ACCOUNT_INVALID;
        std::string label;          // human-readable, e.g. "OANDA-PRIMARY"
        std::string venue;          // "OANDA", "NYSE", "SIM", ...
        int64_t     capital_units  = 0;     // notional capital (currency units)
        bool        live           = false; // true = real money, false = paper
    };

    class StrategyAccountMap {
    public:
        // Register an account. Throws if the id collides or is invalid.
        void add_account(const AccountInfo& info) {
            if (info.id == ACCOUNT_INVALID) {
                throw std::invalid_argument("AccountId must not be ACCOUNT_INVALID");
            }
            if (!accounts_.emplace(info.id, info).second) {
                throw std::invalid_argument("duplicate AccountId: " + std::to_string(info.id));
            }
        }

        // Assign a strategy to an account. The first assignment becomes the
        // primary; subsequent ones are extras. Idempotent.
        void assign(StrategyId sid, AccountId aid) {
            if (accounts_.find(aid) == accounts_.end()) {
                throw std::invalid_argument("unknown AccountId: " + std::to_string(aid));
            }
            auto& list = strategy_to_accounts_[sid];
            for (auto existing : list) {
                if (existing == aid) return; // already assigned
            }
            list.push_back(aid);
            account_to_strategies_[aid].insert(sid);
        }

        // Remove a single (strategy, account) pairing. Safe if absent.
        void unassign(StrategyId sid, AccountId aid) noexcept {
            auto sit = strategy_to_accounts_.find(sid);
            if (sit != strategy_to_accounts_.end()) {
                auto& list = sit->second;
                for (auto it = list.begin(); it != list.end(); ++it) {
                    if (*it == aid) { list.erase(it); break; }
                }
                if (list.empty()) strategy_to_accounts_.erase(sit);
            }
            auto ait = account_to_strategies_.find(aid);
            if (ait != account_to_strategies_.end()) {
                ait->second.erase(sid);
                if (ait->second.empty()) account_to_strategies_.erase(ait);
            }
        }

        // -------------------------------------------------------------------
        // Hot-path lookups
        // -------------------------------------------------------------------
        // The default account a strategy's orders should ride on. Returns
        // ACCOUNT_INVALID if the strategy hasn't been assigned anywhere.
        AccountId primary(StrategyId sid) const noexcept {
            auto it = strategy_to_accounts_.find(sid);
            if (it == strategy_to_accounts_.end() || it->second.empty()) {
                return ACCOUNT_INVALID;
            }
            return it->second.front();
        }

        // All accounts a strategy is allowed to trade on (1:N).
        std::vector<AccountId> accounts_of(StrategyId sid) const {
            auto it = strategy_to_accounts_.find(sid);
            if (it == strategy_to_accounts_.end()) return {};
            return it->second;
        }

        // All strategies attached to one account (N:1).
        std::vector<StrategyId> strategies_of(AccountId aid) const {
            std::vector<StrategyId> out;
            auto it = account_to_strategies_.find(aid);
            if (it == account_to_strategies_.end()) return out;
            out.assign(it->second.begin(), it->second.end());
            return out;
        }

        // Stamp an order with its primary account. Returns false if the
        // strategy has no account assigned (caller should reject the order).
        bool stamp_primary(Order& o) const noexcept {
            const auto sid = static_cast<StrategyId>(o.strategy_id);
            const auto aid = primary(sid);
            if (aid == ACCOUNT_INVALID) return false;
            o.account_id = aid;
            return true;
        }

        // Static account metadata. Returns nullptr for unknown ids.
        const AccountInfo* info(AccountId aid) const noexcept {
            auto it = accounts_.find(aid);
            return it == accounts_.end() ? nullptr : &it->second;
        }

        std::size_t account_count()  const noexcept { return accounts_.size(); }
        std::size_t strategy_count() const noexcept { return strategy_to_accounts_.size(); }

    private:
        std::unordered_map<AccountId, AccountInfo>                       accounts_;
        std::unordered_map<StrategyId, std::vector<AccountId>>           strategy_to_accounts_;
        std::unordered_map<AccountId, std::unordered_set<StrategyId>>    account_to_strategies_;
    };

} // namespace connector
