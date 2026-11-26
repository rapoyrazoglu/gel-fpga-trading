// cl_backtester_defines.vh — register map for the HARDWARE BACKTESTER CL.
//
// The host fills an on-chip tick-mid BRAM (tick_ram[0..1023]) over AXI-Lite,
// programs the ParamBlock scalar fields + n_ticks, pulses START, then polls DONE
// and reads back the 64-bit total_pnl and 32-bit fill count. The on-chip
// hw_backtester streams tick_addr -> tick_mid (1-cycle-late registered read),
// runs the same integer pricing math as the pipeline, and accumulates PnL/fills.
//
// OCL AXI-Lite plumbing (register slice + FSM + handshake + AXI outputs) is REUSED
// VERBATIM from cl_dataplane.sv / the AWS cl_axil_reg_access example; only the
// application register decode changed.
`ifndef CL_AXIL_REG_ACCESS_DEFINES
`define CL_AXIL_REG_ACCESS_DEFINES

// Module name of the CL design (instantiated in the shell top.sv).
`define CL_NAME cl_axil_reg_access

// ---- ParamBlock scalar fields (drive hw_backtester pblk; kill_switch tied 0) --
`define ADDR_INV_SKEW    32'h00   // write => pblk.inv_skew_q31 (signed Q31)
`define ADDR_HALF_SPREAD 32'h04   // write => pblk.half_spread  (signed, ticks)
`define ADDR_MAX_INV     32'h08   // write => pblk.max_inv      (unsigned, lots)
`define ADDR_BASE_QTY    32'h0C   // write => pblk.base_qty     (unsigned)

// ---- run control ------------------------------------------------------------
`define ADDR_NTICKS      32'h10   // write => n_ticks [15:0] (series length, <=1024)
`define ADDR_TICK_WADDR  32'h14   // write => tick_ram write index [9:0] (0..1023)
`define ADDR_TICK_WDATA  32'h18   // write => tick_ram[waddr] <= wdata (signed mid)
`define ADDR_START       32'h1C   // write => 1-cycle start pulse (kicks off a run)
`define ADDR_RESET       32'h20   // write => hold backtester sync reset ~32 cycles

// ---- results (reads) --------------------------------------------------------
`define ADDR_DONE        32'h24   // read => {31'h0, done}
`define ADDR_PNL_LO      32'h28   // read => total_pnl[31:0]  (signed 64b, low)
`define ADDR_PNL_HI      32'h2C   // read => total_pnl[63:32] (signed 64b, high)
`define ADDR_FILLS       32'h30   // read => fills[31:0]

// ---- optional status / readback --------------------------------------------
`define ADDR_STATUS      32'h34   // read => {30'h0, running, done}
`define ADDR_NTICKS_RB   32'h38   // read => n_ticks readback [15:0]
`define ADDR_TICK_RDATA  32'h3C   // read => tick_ram[waddr] readback (uses TICK_WADDR)

`define INVALID_ADDR_RESP 32'hDEADBEEF
`define AXI_PROT_DEFAULT  3'h0
`define AXI_RESP_OKAY     2'b00

`endif
