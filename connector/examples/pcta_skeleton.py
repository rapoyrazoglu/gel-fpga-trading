"""PCTA Python skeleton — what Deniz's PPO+LLM agent looks like as it
plugs into the connector SDK.

This template subclasses ``connector.PyStrategy`` and demonstrates the
contract:

  - Override ``on_event`` / ``on_tick`` / ``on_fill`` as needed
  - Emit orders via ``self.send_order(...)``
  - Engine attribution travels in ``order.strategy_id`` and
    ``order.account_id``

To actually drive this strategy you need an engine (in-process via the
GEL Python module, or cross-process via ``connector.ShmClient``). See
``examples/cross_process/pcta_proc.py`` for the cross-process variant.
"""
from __future__ import annotations

import time

import connector as c


ACCOUNT_PCTA = 0x10


class PCTAAgent(c.PyStrategy):
    """Stub of the real PCTA agent.

    Real implementation:
        - PPO actor-critic with LSTM backbone
        - LLM event-embedding head (FOMC/NFP/CPI text → vector)
        - 25-day dividend window logic (-14d entry, +10d exit)
        - A/B/C cluster ticker selection (refreshed daily)

    For demo: open a long on every HIGH-impact macro event.
    """

    def __init__(self, target_symbol: int, account: int = ACCOUNT_PCTA):
        super().__init__(strategy_id=int(c.StrategyId.PCTA), name="PCTA")
        self.target  = target_symbol
        self.account = account
        self._coid   = 1000
        self.fills   = 0

    def on_event(self, e: c.CalendarEvent) -> None:
        if e.impact != int(c.EventImpact.HIGH):
            return
        if e.kind not in (
            int(c.EventKind.FOMC),
            int(c.EventKind.NFP),
            int(c.EventKind.CPI),
        ):
            return

        order = c.Order.market(sid=self.target, qty=1000, side=c.Side.BUY)
        order.ts_ns           = time.time_ns()
        order.client_order_id = self._coid
        order.strategy_id     = int(c.StrategyId.PCTA)
        order.account_id      = self.account
        self._coid += 1
        self.send_order(order)

    def on_fill(self, r: c.ExecReport) -> None:
        if r.status == int(c.ExecStatus.FILLED):
            self.fills += 1
            # In production: this is where the PPO reward signal is computed.


if __name__ == "__main__":
    # Construction smoke-test — actual driving happens in pcta_proc.py
    # (cross-process) or via the GEL Python Engine (in-process).
    agent = PCTAAgent(target_symbol=1)
    print(f"PCTAAgent constructed: name={agent.name()} fills={agent.fills}")
