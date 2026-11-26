#!/usr/bin/env bash
# tools/new_strategy.sh
#
# Scaffold a new GEL strategy in one command.
#
# Usage:   ./tools/new_strategy.sh <ClassName> <STRATEGY_ID_NUM>
# Example: ./tools/new_strategy.sh MomentumScalper 3
#
# This writes include/gel/strategies/<snake_case>.hpp implementing
# IStrategy and gives you the registration line to paste in main.

set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <ClassName> <STRATEGY_ID_NUM>" >&2
    echo "  ClassName       — CamelCase, e.g. MomentumScalper" >&2
    echo "  STRATEGY_ID_NUM — integer 3..255, must be unique in StrategyId enum" >&2
    exit 1
fi

CLASS="$1"
ID_NUM="$2"

# CamelCase → snake_case (portable: works on BSD and GNU sed)
SNAKE=$(printf '%s' "$CLASS" | sed -E 's/([A-Z])/_\1/g' | sed 's/^_//' | tr 'A-Z' 'a-z')
HEADER="include/gel/strategies/${SNAKE}.hpp"
ENUM_NAME=$(printf '%s' "$CLASS" | sed -E 's/([A-Z])/_\1/g' | sed 's/^_//' | tr 'a-z' 'A-Z')

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO_ROOT"

if [[ -e "$HEADER" ]]; then
    echo "ERROR: $HEADER already exists" >&2
    exit 2
fi

# Make sure the strategy_id slot is free (very crude check).
if grep -q "= ${ID_NUM}," include/gel/sdk/wire.hpp 2>/dev/null; then
    echo "WARNING: StrategyId=${ID_NUM} already used. Edit include/gel/sdk/wire.hpp first." >&2
fi

cat > "$HEADER" <<EOF
// ${HEADER}
//
// ${CLASS} — TODO(strategy author): one-sentence description of the alpha.
//
// Implements gel::sdk::IStrategy. Registered with GelEngine alongside any
// other strategy; risk, formatting, routing and drop-copy are shared.
//
#pragma once

#include "gel/sdk/strategy.hpp"
#include "gel/platform/telemetry.hpp"   // for now_ns()
#include <atomic>

namespace gel::strategies {

    using gel::sdk::IStrategy;
    using gel::sdk::OrderSink;
    using gel::sdk::Order;
    using gel::sdk::Tick;
    using gel::sdk::BookUpdate;
    using gel::sdk::CalendarEvent;
    using gel::sdk::ExecReport;
    using gel::sdk::StrategyId;
    using gel::sdk::Side;
    using gel::sdk::OrdType;
    using gel::sdk::TIF;

    struct ${CLASS}Config {
        // TODO: strategy parameters
    };

    class ${CLASS} final : public IStrategy {
    public:
        explicit ${CLASS}(${CLASS}Config cfg = {}) : cfg_(cfg) {}

        StrategyId  id()   const noexcept override {
            // Make sure StrategyId::${ENUM_NAME} = ${ID_NUM} exists in
            // include/gel/sdk/wire.hpp.
            return static_cast<StrategyId>(${ID_NUM});
        }
        std::string name() const override { return "${CLASS}"; }

        void on_start(OrderSink& sink) override { sink_ = &sink; }
        void on_stop()  override                { sink_ = nullptr; }

        // Hot path callbacks — implement only the ones your alpha needs.
        void on_tick (const Tick&          /*t*/) override { /* TODO */ }
        void on_book (const BookUpdate&    /*b*/) override { /* TODO */ }
        void on_event(const CalendarEvent& /*e*/) override { /* TODO */ }
        void on_fill (const ExecReport&    /*r*/) override { /* TODO */ }

        void on_kill_switch(gel::sdk::RejectReason) override { active_ = false; }
        void on_resume() override                            { active_ = true;  }

    private:
        // Helper: prebuilt Order skeleton for this strategy. Caller fills
        // symbol_id, price, qty, side, ord_type, tif and calls sink_->send().
        Order make_order() noexcept {
            Order o{};
            o.ts_ns           = gel::platform::now_ns();
            o.client_order_id = ++next_coid_;
            o.strategy_id     = static_cast<uint32_t>(id());
            return o;
        }

        ${CLASS}Config        cfg_;
        OrderSink*            sink_   = nullptr;
        std::atomic<bool>     active_{true};
        uint64_t              next_coid_{0};
    };

} // namespace gel::strategies
EOF

cat <<MSG

Created  $HEADER

Next steps:
  1. Add StrategyId::${ENUM_NAME} = ${ID_NUM} to include/gel/sdk/wire.hpp
  2. Implement on_tick / on_book / on_event in the new file.
  3. Register in your main:

       #include "gel/strategies/${SNAKE}.hpp"
       auto s = std::make_shared<gel::strategies::${CLASS}>();
       engine.register_strategy(s);

MSG
