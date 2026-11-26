// include/gel/platform/symbol_table.hpp
//
// Block ④ in deck slide 6 — ticker → symbol_id → venue lookup.
// Strategies see only the SymbolId on the wire; humans see the ticker
// when reading audit logs. Registration is one-shot at startup; the hot
// path is read-only and lock-free.
//
#pragma once

#include "connector/wire.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

namespace gel::platform {

    using connector::SymbolId;
    using connector::SYMBOL_INVALID;

    enum class Venue : uint8_t {
        UNKNOWN = 0,
        OANDA   = 1,
        NYSE    = 2,
        NASDAQ  = 3,
        SIM     = 255,
    };

    struct SymbolMeta {
        SymbolId    id           = SYMBOL_INVALID;
        Venue       venue        = Venue::UNKNOWN;
        char        ticker[16]   = {0};   // null-terminated
        int64_t     tick_size_ticks = 1;  // minimum price increment in our scale
        int64_t     lot_size      = 1;    // minimum quantity
    };

    class SymbolTable {
    public:
        // Registration is not hot-path. Call only from setup.
        SymbolId register_symbol(std::string_view ticker,
                                 Venue            venue,
                                 int64_t          tick_size_ticks = 1,
                                 int64_t          lot_size        = 1) {
            if (auto it = by_ticker_.find(std::string(ticker)); it != by_ticker_.end()) {
                return it->second;
            }
            const SymbolId id = next_id_.fetch_add(1, std::memory_order_relaxed);
            if (id >= MAX_SYMBOLS) return SYMBOL_INVALID;

            SymbolMeta m;
            m.id    = id;
            m.venue = venue;
            m.tick_size_ticks = tick_size_ticks;
            m.lot_size        = lot_size;
            const auto n = std::min<size_t>(ticker.size(), sizeof(m.ticker) - 1);
            std::memcpy(m.ticker, ticker.data(), n);

            meta_[id] = m;
            by_ticker_.emplace(std::string(ticker), id);
            return id;
        }

        // Hot path — returns nullptr if the id was never registered.
        [[nodiscard]] const SymbolMeta* lookup(SymbolId id) const noexcept {
            if (id == SYMBOL_INVALID || id >= next_id_.load(std::memory_order_relaxed))
                return nullptr;
            return &meta_[id];
        }

        [[nodiscard]] SymbolId find(std::string_view ticker) const {
            auto it = by_ticker_.find(std::string(ticker));
            return it == by_ticker_.end() ? SYMBOL_INVALID : it->second;
        }

        [[nodiscard]] size_t size() const noexcept {
            return next_id_.load(std::memory_order_relaxed);
        }

    private:
        static constexpr size_t MAX_SYMBOLS = 4096;

        // SymbolId 0 is reserved as INVALID.
        std::atomic<SymbolId>                 next_id_{1};
        std::array<SymbolMeta, MAX_SYMBOLS>   meta_{};
        std::unordered_map<std::string, SymbolId> by_ticker_;
    };

} // namespace gel::platform
