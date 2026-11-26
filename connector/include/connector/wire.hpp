// connector/wire.hpp
//
// Wire types — the binary contract between every strategy and the
// shared execution engine.
//
// Design goals:
//   - POD structs only. trivially copyable, standard layout.
//   - Fixed-size fields. No std::string/vector in the hot path.
//   - Cacheline-aligned (64 B) so SHM ring buffer slots don't false-share.
//   - Fixed-point prices (int64 ticks) so VHDL doesn't have to parse floats.
//   - uint64 nanosecond timestamps from a single epoch (CLOCK_REALTIME).
//   - Endian-neutral on the host; the FPGA NIC path will byte-swap as needed.
//
// Anything that crosses the strategy/engine boundary lives in this file.
//
#pragma once

#include <cstdint>
#include <type_traits>

namespace connector {

    // -------------------------------------------------------------------
    // Strategy identity
    // -------------------------------------------------------------------
    // Every order, fill, telemetry record carries a strategy_id so the
    // engine can attribute capital, risk and audit to the right module.
    enum class StrategyId : uint32_t {
        UNKNOWN      = 0,
        PCTA         = 1,
        MARKET_MAKER = 2,
        // Reserve 3..255 for production strategies.
        // 256+ for research / paper-only.
    };

    // -------------------------------------------------------------------
    // Account identity
    // -------------------------------------------------------------------
    // Many strategies → many accounts (N:M). The engine routes orders to
    // the right broker account based on this field. Strategies may share
    // an account (multi-strat on a single book) or own several. Mapping
    // policy lives in connector/account_map.hpp.
    using AccountId = uint32_t;
    inline constexpr AccountId ACCOUNT_INVALID = 0u;
    inline constexpr AccountId ACCOUNT_DEFAULT = 0u;  // alias — both mean "unset"

    // -------------------------------------------------------------------
    // Order book primitives
    // -------------------------------------------------------------------
    enum class Side    : uint8_t { BUY = 0, SELL = 1 };

    // ENTER variants (MARKET..POST_ONLY) place a brand-new order. CANCEL and
    // REPLACE reference an already-placed order via `ref_order_id`:
    //   CANCEL  : ref_order_id = coid to cancel; quantity ignored; price_ticks=-1.
    //   REPLACE : ref_order_id = old coid; client_order_id = fresh coid;
    //             new price/quantity in the usual fields. Atomic on engines
    //             that support it; falls back to cancel-then-place on engines
    //             that don't.
    // The encoder emits a 56-byte OUCH frame; all fields including
    // `ref_order_id` are encoded unconditionally (ENTER uses ref_order_id=0),
    // and the parity test covers the full frame.
    enum class OrdType : uint8_t {
        MARKET    = 0,
        LIMIT     = 1,
        IOC       = 2,
        FOK       = 3,
        POST_ONLY = 4,
        CANCEL    = 5,
        REPLACE   = 6,
    };

    enum class TIF     : uint8_t { DAY = 0, IOC = 1, FOK = 2, GTC = 3 };

    enum class OrdFlags : uint8_t {
        NONE        = 0,
        HIDDEN      = 1 << 0,
        ICEBERG     = 1 << 1,
        REDUCE_ONLY = 1 << 2,
    };

    // -------------------------------------------------------------------
    // Symbol identity
    // -------------------------------------------------------------------
    // The engine's SymbolTable maps human tickers ("EURUSD", "AAPL") to a
    // 32-bit symbol_id. After registration only the id travels on the wire.
    using SymbolId = uint32_t;
    inline constexpr SymbolId SYMBOL_INVALID = 0u;

    // -------------------------------------------------------------------
    // Fixed-point pricing
    // -------------------------------------------------------------------
    // price_ticks = price * 10^PRICE_SCALE.
    // Default scale is 8 — covers FX (EUR/USD 1.07543 → 107'543'000) and
    // equities (AAPL 187.42 → 18'742'000'000) without overflow in int64.
    inline constexpr int     PRICE_SCALE         = 8;
    inline constexpr int64_t PRICE_TICK_PER_UNIT = 100'000'000LL; // 10^8

    inline constexpr int64_t to_ticks(double price) noexcept {
        return static_cast<int64_t>(price * static_cast<double>(PRICE_TICK_PER_UNIT));
    }
    inline constexpr double from_ticks(int64_t ticks) noexcept {
        return static_cast<double>(ticks) / static_cast<double>(PRICE_TICK_PER_UNIT);
    }

    // -------------------------------------------------------------------
    // Order — strategy → engine
    // -------------------------------------------------------------------
    // Exactly one cacheline (64 B). FPGA reads a single AXI burst.
    //
    // Byte layout (do NOT reorder — the HLS encoder reads fixed offsets):
    //    0-7   ts_ns
    //    8-15  client_order_id
    //   16-19  strategy_id
    //   20-23  symbol_id
    //   24-31  price_ticks   (-1 = MARKET)
    //   32-39  quantity      (signed: + buy / - sell, see also `side`)
    //   40     side
    //   41     ord_type
    //   42     tif
    //   43     flags
    //   44-47  risk_budget_bps
    //   48-51  account_id    (host-side routing only, not on wire frame)
    //   52-59  ref_order_id  (NEW — used by OrdType::CANCEL / REPLACE only)
    //   60-63  reserved[4]
    #pragma pack(push, 1)
    struct alignas(64) Order {
        uint64_t  ts_ns;
        uint64_t  client_order_id;
        uint32_t  strategy_id;
        SymbolId  symbol_id;
        int64_t   price_ticks;
        int64_t   quantity;
        uint8_t   side;
        uint8_t   ord_type;
        uint8_t   tif;
        uint8_t   flags;
        uint32_t  risk_budget_bps;
        AccountId account_id;
        uint64_t  ref_order_id;     // CANCEL/REPLACE: original coid to operate on
        uint8_t   reserved[4];
    };
    #pragma pack(pop)
    static_assert(sizeof(Order) == 64, "Order must be exactly one cacheline");
    static_assert(std::is_trivially_copyable_v<Order>, "Order must be POD");
    static_assert(std::is_standard_layout_v<Order>,    "Order must be standard-layout");

    // -------------------------------------------------------------------
    // ExecutionReport — engine → strategy (drop-copy)
    // -------------------------------------------------------------------
    enum class ExecStatus : uint8_t {
        ACCEPTED = 0,   // passed risk, sent to wire
        REJECTED = 1,   // killed by risk
        PARTIAL  = 2,   // partial fill
        FILLED   = 3,   // fully filled
        CANCELED = 4,
        EXPIRED  = 5,
    };

    enum class RejectReason : uint8_t {
        NONE           = 0,
        FAT_FINGER     = 1,
        POSITION_LIMIT = 2,
        THROTTLE       = 3,
        UNKNOWN_SYMBOL = 4,
        KILL_SWITCH    = 5,
        BAD_PRICE      = 6,
        BAD_QTY        = 7,
        STALE_ORDER    = 8,
    };

    // Layout matches GEL's existing wire.hpp byte-for-byte (account_id is
    // packed right after `side` with no pad — pragma pack(1) makes it work
    // even though it's 4-byte unaligned).
    #pragma pack(push, 1)
    struct alignas(64) ExecReport {
        uint64_t  ts_ns;
        uint64_t  client_order_id;
        uint64_t  exchange_order_id;
        uint32_t  strategy_id;
        SymbolId  symbol_id;
        int64_t   fill_price_ticks;
        int64_t   fill_quantity;
        uint8_t   status;
        uint8_t   reject_reason;
        uint8_t   side;
        AccountId account_id;
        uint8_t   reserved[9];
    };
    #pragma pack(pop)
    static_assert(sizeof(ExecReport) == 64, "ExecReport must be exactly one cacheline");
    static_assert(std::is_trivially_copyable_v<ExecReport>, "ExecReport must be POD");
    static_assert(std::is_standard_layout_v<ExecReport>,    "ExecReport must be standard-layout");

    // -------------------------------------------------------------------
    // Tick — market data → strategy
    // -------------------------------------------------------------------
    // 32-byte slot: smaller because tick rate is the highest-volume stream.
    // Strategies that need full L2 subscribe to BookUpdate (below).
    #pragma pack(push, 1)
    struct alignas(32) Tick {
        uint64_t ts_ns;
        SymbolId symbol_id;
        uint32_t seq;             // per-symbol monotonically increasing
        int64_t  bid_ticks;
        int64_t  ask_ticks;
    };
    #pragma pack(pop)
    static_assert(sizeof(Tick) == 32, "Tick must be exactly half a cacheline");
    static_assert(std::is_trivially_copyable_v<Tick>, "Tick must be POD");

    // L2 book level — used by Market-Maker for inventory/spread decisions.
    #pragma pack(push, 1)
    struct BookLevel {
        int64_t price_ticks;
        int64_t quantity;
    };
    #pragma pack(pop)
    static_assert(sizeof(BookLevel) == 16);

    // Top-N book snapshot — fixed depth keeps the slot size FPGA-friendly.
    inline constexpr int BOOK_DEPTH = 5;

    #pragma pack(push, 1)
    struct alignas(64) BookUpdate {
        uint64_t  ts_ns;
        SymbolId  symbol_id;
        uint32_t  seq;
        BookLevel bids[BOOK_DEPTH];     // 80
        BookLevel asks[BOOK_DEPTH];     // 80
        uint8_t   reserved[16];         // pad to 192 (3 cachelines)
    };
    #pragma pack(pop)
    static_assert(sizeof(BookUpdate) == 192, "BookUpdate sized for 5-level book (3 cachelines)");
    static_assert(std::is_trivially_copyable_v<BookUpdate>, "BookUpdate must be POD");

    // -------------------------------------------------------------------
    // CalendarEvent — feeds PCTA
    // -------------------------------------------------------------------
    enum class EventKind : uint8_t {
        UNKNOWN  = 0,
        DIVIDEND = 1,    // ex-dividend date
        FOMC     = 2,    // Fed rate decision
        NFP      = 3,    // non-farm payrolls
        CPI      = 4,    // inflation print
        ECB      = 5,
        EARNINGS = 6,
    };

    enum class EventImpact : uint8_t { LOW = 0, MEDIUM = 1, HIGH = 2 };

    #pragma pack(push, 1)
    struct alignas(64) CalendarEvent {
        uint64_t event_ts_ns;     // when the event happens
        uint64_t ingest_ts_ns;    // when the engine received the row
        SymbolId symbol_id;       // 0 = market-wide event
        uint8_t  kind;
        uint8_t  impact;
        uint8_t  reserved[2];
        int64_t  consensus_x1e6;  // consensus, scaled (e.g. CPI %)
        int64_t  actual_x1e6;     // actual; INT64_MIN if not yet released
        int64_t  previous_x1e6;   // prior period
        uint8_t  pad[16];         // pad to 64
    };
    #pragma pack(pop)
    static_assert(sizeof(CalendarEvent) == 64);
    static_assert(std::is_trivially_copyable_v<CalendarEvent>);

    // -------------------------------------------------------------------
    // Telemetry — per-hop timestamp record
    // -------------------------------------------------------------------
    enum class Hop : uint8_t {
        STRATEGY_EMIT = 0,
        IPC_DEQUEUE   = 1,
        RISK_PASS     = 2,
        ROUTE_PICK    = 3,
        ENCODE_DONE   = 4,
        WIRE_OUT      = 5,
        EXEC_RECV     = 6,
        DROP_COPY     = 7,
    };

    #pragma pack(push, 1)
    struct alignas(64) TelemetryRecord {
        uint64_t ts_ns;
        uint64_t client_order_id;
        uint32_t strategy_id;
        uint8_t  hop;
        uint8_t  reserved[43];
    };
    #pragma pack(pop)
    static_assert(sizeof(TelemetryRecord) == 64);
    static_assert(std::is_trivially_copyable_v<TelemetryRecord>, "TelemetryRecord must be POD");

} // namespace connector
