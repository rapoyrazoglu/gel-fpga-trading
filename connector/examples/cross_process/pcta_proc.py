"""Cross-process PCTA strategy stub.

Runs in its own Python interpreter, connects to the GEL engine daemon
over a pair of mmap'd ring files. This is what the real PCTA agent
will look like once its PPO + LLM stack is in the loop — every
``send_order`` flows through the connector's SHM transport into GEL,
and every fill comes back via ``poll_report``.

Usage::

    # Terminal A — start the engine daemon (creates the ring files):
    ./build/examples/cross_process/gel_daemon \\
        /tmp/orders.shm /tmp/reports.shm

    # Terminal B — start this strategy:
    PYTHONPATH=python python3 examples/cross_process/pcta_proc.py \\
        /tmp/orders.shm /tmp/reports.shm
"""
from __future__ import annotations

import sys
import time

import connector as c

ACCOUNT_PCTA = 0x10  # arbitrary; engine uses it for routing/audit
N_ORDERS     = 100


def main(orders_path: str, reports_path: str) -> int:
    client = c.ShmClient(orders_path, reports_path)
    print(f"pcta_proc: connected to {orders_path} / {reports_path}")

    sent = 0
    fills = 0
    rejected = 0

    while sent < N_ORDERS or (fills + rejected) < N_ORDERS:
        if sent < N_ORDERS:
            o = c.Order.market(sid=1, qty=1000, side=c.Side.BUY)
            o.ts_ns           = time.time_ns()
            o.client_order_id = sent + 1
            o.strategy_id     = int(c.StrategyId.PCTA)
            o.account_id      = ACCOUNT_PCTA
            if client.send_order(o):
                sent += 1
            else:
                # Ring is full — back off a hair and retry.
                time.sleep(0.001)

        r = client.poll_report()
        while r is not None:
            if r.status == int(c.ExecStatus.FILLED):
                fills += 1
            elif r.status == int(c.ExecStatus.REJECTED):
                rejected += 1
            else:
                # PARTIAL / CANCELED / EXPIRED — count as terminal here.
                rejected += 1
            r = client.poll_report()

    print(f"pcta_proc: sent={sent} fills={fills} rejected={rejected}")
    return 0 if fills == N_ORDERS else 1


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
