# AXI-Lite register map

The data-plane custom logic sits behind the AWS shell's OCL AXI-Lite BAR.
Defined in [`cl_dataplane_defines.vh`](../../Market-Maker/fpga/cl_dataplane/cl_dataplane_defines.vh).
All registers are 32 bits. An unmapped address reads `0xDEADBEEF`.

## Parameter block load

| Address | Access | Register | Function |
|---|---|---|---|
| 0x00 | W | `PB_ADDR` | Word index, 0 to 15 |
| 0x04 | W | `PB_DATA` | Writes the word at `PB_ADDR` into the loader |
| 0x08 | W | `PB_COMMIT` | Verify and swap in the new block |

## Market-data frame in

| Address | Access | Register | Function |
|---|---|---|---|
| 0x0C | W | `RX_WADDR` | Beat index, 0 to 31 |
| 0x10 | W | `RX_DATA_LO` | Low 32 bits of the 64-bit beat |
| 0x14 | W | `RX_DATA_HI` | High 32 bits; the write stores the beat |
| 0x18 | W | `RX_KEEP` | Bits 7:0 byte enables, bit 8 last beat |
| 0x1C | W | `RX_NBEATS` | Beats in the frame |
| 0x20 | W | `RX_GO` | Start replay and the latency counter |

## Order frame out

| Address | Access | Register | Function |
|---|---|---|---|
| 0x24 | R | `TX_COUNT` | Beats captured |
| 0x28 | W | `TX_RADDR` | Beat index to read |
| 0x2C | R | `TX_DATA_LO` | Low 32 bits |
| 0x30 | R | `TX_DATA_HI` | High 32 bits |
| 0x34 | R | `TX_KEEP` | Last flag and byte enables |

## Configuration and status

| Address | Access | Register | Function |
|---|---|---|---|
| 0x38 | W | `SESSION` | Session id placed in outgoing frames |
| 0x3C | W | `UDP_PORT` | Destination port accepted by the receiver |
| 0x40 | R | `QCPU` | Inventory as seen by the chip |
| 0x44 | R | `LOADERR` | 0 ok, 1 tag mismatch, 2 stale |
| 0x48 | R | `LATENCY` | Cycles from first data beat in to order beat out |
| 0x4C | R | `STATUS` | Transmit done, replay busy |
| 0x50 | W | `RESET` | Reset the data plane and counters |
| 0x54 | W | `BOOK_CLEAR` | Empty the order book; inventory and parameters stay |
| 0x58 | R | `DBG_QC` | Clamped inventory fed to the multiplier |

One cycle is 4 ns. `LATENCY` reads 20 on the shipped design.
