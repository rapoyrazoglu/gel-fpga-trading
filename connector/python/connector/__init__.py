"""connector — strategy ↔ engine SDK (Python facade).

Thin re-export layer over the pybind11 module ``_connector_native``.
Lets strategy code do::

    import connector

instead of fishing through the C-style native module.

Typical usage — cross-process Python strategy talking to a C++ engine
daemon::

    import connector as c

    client = c.ShmClient("/tmp/orders.shm", "/tmp/reports.shm")
    o = c.Order.market(sid=1, qty=100, side=c.Side.BUY)
    client.send_order(o)
    while True:
        r = client.poll_report()
        if r is None:
            break
        ...

Or as a subclassable in-process IStrategy via ``PyStrategy``.
"""

from ._connector_native import (
    # Enums
    StrategyId,
    Side,
    OrdType,
    TIF,
    EventKind,
    EventImpact,
    ExecStatus,
    RejectReason,
    # Wire types
    Order,
    Tick,
    CalendarEvent,
    ExecReport,
    # Strategy
    PyStrategy,
    # IPC
    ShmClient,
    # Accounts
    AccountInfo,
    StrategyAccountMap,
    # Helpers
    to_ticks,
    from_ticks,
)

__all__ = [
    "StrategyId",
    "Side",
    "OrdType",
    "TIF",
    "EventKind",
    "EventImpact",
    "ExecStatus",
    "RejectReason",
    "Order",
    "Tick",
    "CalendarEvent",
    "ExecReport",
    "PyStrategy",
    "ShmClient",
    "AccountInfo",
    "StrategyAccountMap",
    "to_ticks",
    "from_ticks",
]
