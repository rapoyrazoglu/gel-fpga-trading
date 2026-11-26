// include/gel/platform/account_registry.hpp
//
// AccountRegistry — every trading account GEL knows about.
//
// One account = (name, broker handle, balance, kill switch, risk budget).
// Strategies can run on multiple accounts; multiple strategies can share
// an account. Risk + PnL + capital allocation are keyed on (strategy, account).
//
#pragma once

#include "broker_client.hpp"
#include "connector/wire.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace gel::platform {

    using connector::AccountId;
    using connector::ACCOUNT_INVALID;

    struct AccountState {
        AccountId   id            = ACCOUNT_INVALID;
        char        name[32]      = {0};        // human-readable
        BrokerKind  broker_kind   = BrokerKind::UNKNOWN;
        // Live balance, reserved capital, atomic so the dynamic allocator
        // can update without locking the hot path.
        std::atomic<int64_t> balance_ticks{0};      // total notional in ticks
        std::atomic<int64_t> reserved_ticks{0};     // currently in flight
        std::atomic<bool>    kill_switch{false};

        AccountState() = default;
        AccountState(const AccountState& other) {
            id          = other.id;
            std::memcpy(name, other.name, sizeof(name));
            broker_kind = other.broker_kind;
            balance_ticks .store(other.balance_ticks .load(std::memory_order_relaxed));
            reserved_ticks.store(other.reserved_ticks.load(std::memory_order_relaxed));
            kill_switch   .store(other.kill_switch   .load(std::memory_order_relaxed));
        }
    };

    class AccountRegistry {
    public:
        // Setup-only. Returns the new AccountId, or ACCOUNT_INVALID on failure.
        AccountId register_account(std::string_view name,
                                   std::shared_ptr<IBrokerClient> broker,
                                   int64_t initial_balance_ticks = 0) {
            const auto id = next_id_.fetch_add(1, std::memory_order_relaxed);
            if (id >= MAX_ACCOUNTS) return ACCOUNT_INVALID;

            auto& state = states_[id];
            state.id          = id;
            state.broker_kind = broker ? broker->kind() : BrokerKind::UNKNOWN;
            const auto n = std::min<size_t>(name.size(), sizeof(state.name) - 1);
            std::memcpy(state.name, name.data(), n);
            state.balance_ticks.store(initial_balance_ticks, std::memory_order_relaxed);

            brokers_[id]   = std::move(broker);
            by_name_[std::string(name)] = id;
            return id;
        }

        [[nodiscard]] AccountState* get(AccountId id) noexcept {
            if (id == ACCOUNT_INVALID || id >= next_id_.load(std::memory_order_relaxed))
                return nullptr;
            return &states_[id];
        }
        [[nodiscard]] const AccountState* get(AccountId id) const noexcept {
            if (id == ACCOUNT_INVALID || id >= next_id_.load(std::memory_order_relaxed))
                return nullptr;
            return &states_[id];
        }

        [[nodiscard]] IBrokerClient* broker(AccountId id) noexcept {
            return id < brokers_.size() ? brokers_[id].get() : nullptr;
        }

        [[nodiscard]] AccountId find(std::string_view name) const {
            auto it = by_name_.find(std::string(name));
            return it == by_name_.end() ? ACCOUNT_INVALID : it->second;
        }

        [[nodiscard]] size_t size() const noexcept {
            return next_id_.load(std::memory_order_relaxed);
        }

        // Iterate over every registered account. Used by the dynamic
        // capital allocator and the multi-account telemetry summary.
        template <typename F>
        void for_each(F&& f) {
            const auto n = next_id_.load(std::memory_order_relaxed);
            for (AccountId i = 1; i < n; ++i) f(states_[i]);
        }

    private:
        static constexpr size_t MAX_ACCOUNTS = 64;

        // Slot 0 is reserved as INVALID.
        std::atomic<AccountId>                          next_id_{1};
        std::array<AccountState,                        MAX_ACCOUNTS> states_{};
        std::array<std::shared_ptr<IBrokerClient>,      MAX_ACCOUNTS> brokers_{};
        std::unordered_map<std::string, AccountId>      by_name_;
    };

} // namespace gel::platform
