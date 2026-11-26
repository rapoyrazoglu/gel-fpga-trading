// cl_multiasset_defines.vh — register map for the MULTI-ASSET parallel pricing CL.
// N asset lanes priced in the same cycle. Each lane has its own (s, q, ParamBlock)
// input registers and its own (bid, ask, qty, cancel) read registers. The host
// pokes every lane's inputs, pulses ADDR_STROBE (valid_in), then peeks each lane.
`ifndef CL_AXIL_REG_ACCESS_DEFINES
`define CL_AXIL_REG_ACCESS_DEFINES

// Put module name of the CL design here. Used to instantiate in top.sv.
`define CL_NAME cl_axil_reg_access

// Number of parallel asset lanes. MUST equal pricing_engine_multi_flat generic N
// (default 4) and pricing_engine_multi generic N.
`define MA_N_LANES  4

// ---------------------------------------------------------------------------
// PER-LANE INPUT registers (write). Byte address = lane*LANE_STRIDE + field.
//   lane = addr[6:5]  (0..3),  field word = addr[4:2].
//   lane 0 = 0x00, lane 1 = 0x20, lane 2 = 0x40, lane 3 = 0x60  (0x00..0x7F)
// ---------------------------------------------------------------------------
`define MA_LANE_STRIDE     32'h20   // bytes between lane input blocks (8 words)
`define MA_LANE_S          32'h00   // signed32 price  s_in
`define MA_LANE_Q          32'h04   // signed32 inventory q_in
`define MA_LANE_INV_SKEW   32'h08   // signed32 inv_skew_q31
`define MA_LANE_HALF_SPREAD 32'h0C  // signed32 half_spread
`define MA_LANE_MAX_INV    32'h10   // unsigned32 max_inv (inventory cap)
`define MA_LANE_BASE_QTY   32'h14   // unsigned32 base_qty (order size)
`define MA_LANE_SKEWS      32'h18   // [15:0]=bid_skew(s16), [31:16]=ask_skew(s16)
`define MA_LANE_KILL       32'h1C   // [0]=kill_switch

// ---------------------------------------------------------------------------
// CONTROL / STATUS (bit8 = 0, addr >= 0x80)
// ---------------------------------------------------------------------------
`define ADDR_STROBE   32'h80   // write => pulse valid_in to ALL lanes (1 cycle)
`define ADDR_STATUS   32'h84   // read  => {..., quote_valid(bit1), result_ready(bit0)}
`define ADDR_RESET    32'h88   // write => reset the pricing pipeline + counters
`define ADDR_N_LANES  32'h8C   // read  => N (compile-time lane count, for host self-check)

// ---------------------------------------------------------------------------
// PER-LANE OUTPUT registers (read). Byte address = 0x100 + lane*OUT_STRIDE + fld.
//   bit8 = 1 selects the output region, lane = addr[5:4], field word = addr[3:2].
//   lane 0 = 0x100, lane 1 = 0x110, lane 2 = 0x120, lane 3 = 0x130  (0x100..0x13F)
// ---------------------------------------------------------------------------
`define MA_OUT_BASE    32'h100  // output region base
`define MA_OUT_STRIDE  32'h10   // bytes between lane output blocks (4 words)
`define MA_OUT_BID     32'h00   // signed32 bid
`define MA_OUT_ASK     32'h04   // signed32 ask
`define MA_OUT_QTY     32'h08   // unsigned32 qty
`define MA_OUT_CANCEL  32'h0C   // [0]=cancel_only

// ---- status bits (from cl_multiasset.sv) ----
//   {30'h0, quote_valid_live, result_ready}
`define MA_STATUS_READY      32'h1   // bit0: sticky result-ready (set on quote_valid)
`define MA_STATUS_QV_LIVE    32'h2   // bit1: live quote_valid strobe

`define INVALID_ADDR_RESP 32'hDEADBEEF
`define AXI_PROT_DEFAULT  3'h0
`define AXI_RESP_OKAY     2'b00

`endif
