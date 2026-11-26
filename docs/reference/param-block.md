# Parameter block

The 64-byte block the host sends to the FPGA about ten times a second.
Defined in [`host/ipc/param_block.h`](../../Market-Maker/host/ipc/param_block.h);
every offset is pinned with `static_assert`.

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | `inv_skew_q31` | Inventory skew per lot, Q1.31 signed fixed point |
| 4 | 4 | `half_spread` | Half the quoted spread, in ticks |
| 8 | 4 | `max_inv` | Position limit; beyond it quotes are pulled |
| 12 | 4 | `base_qty` | Order size |
| 16 | 1 | `regime_id` | 0 ranging, 1 trend up, 2 trend down, 3 high volatility, 4 event imminent |
| 17 | 1 | `kill_switch` | 1 means cancel everything and place nothing |
| 18 | 2 | `bid_skew_ticks` | Signed regime adjustment to the bid |
| 20 | 2 | `ask_skew_ticks` | Signed regime adjustment to the ask |
| 22 | 2 | `requote_ticks` | Price move that triggers a replace |
| 24 | 8 | `epoch_ns` | Host time when the block was built |
| 32 | 16 | reserved | Zero; covered by the tag |
| 48 | 16 | `hmac` | HMAC-SHA256 over bytes 0 to 47, truncated to 16 bytes |

## Rules the receiver applies

- **Authenticity.** A block whose tag does not verify is rejected and the kill
  switch is asserted.
- **Staleness.** A block older than 500 ms asserts the kill switch. A host that
  stops talking stops the quoting.
- **Quote age.** A resting quote older than 50 ms is cancelled and re-quoted.

## The quote computed from it

With mid price `s` and signed inventory `q`:

```
qc   = clamp(q, -max_inv, +max_inv)
skew = (qc * inv_skew_q31 + 2^30) >> 31
r    = s - skew
bid  = saturate32(r - half_spread - bid_skew_ticks)
ask  = saturate32(r + half_spread + ask_skew_ticks)
cancel_only = kill_switch or |q| > max_inv
```

## Q1.31

A 32-bit signed integer read as a fraction: the value is `integer / 2^31`, so
the range is −1 up to just under +1. `0.5` is `0x40000000`.
