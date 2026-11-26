// =============================================================================
// cl_axil_reg_access.sv  —  HARDWARE BACKTESTER on AWS F2 (VU47P)
//
// Puts the hw_backtester (fpga/rtl/hw_backtester.vhd) on silicon behind the shell
// OCL AXI-Lite BAR. The host:
//   1. fills the on-chip tick-mid BRAM tick_ram[0..1023] (ADDR_TICK_WADDR then
//      ADDR_TICK_WDATA per sample),
//   2. programs the ParamBlock scalar fields (inv_skew, half_spread, max_inv,
//      base_qty; kill_switch tied 0) and n_ticks,
//   3. pulses ADDR_START,
//   4. polls ADDR_DONE, then reads ADDR_PNL_LO/HI (signed 64-bit total_pnl) and
//      ADDR_FILLS.
//
// hw_backtester drives tick_addr this cycle and expects the addressed mid on
// tick_mid ONE cycle later (synchronous / registered BRAM read). We honour that
// by registering tick_ram[tick_addr] into tick_mid_r (see below) — exactly the
// synchronous BRAM model in tb_hw_backtester.vhd.
//
// hw_backtester has a `pblk : param_block_t` RECORD port; Vivado's mixed-language
// boundary cannot map a VHDL composite type to a SystemVerilog instance, so we
// bind the FLAT-PORT wrapper hw_backtester_wrap (fpga/rtl/hw_backtester_wrap.vhd),
// which assembles the record and instantiates hw_backtester BY NAME.
//
// The OCL AXI-Lite plumbing (register slice + FSM + handshake + address capture +
// AXI output signals) is reused VERBATIM from cl_dataplane.sv / the AWS
// cl_axil_reg_access example; only the application register decode changed.
// =============================================================================

`include "cl_axil_reg_access_defines.vh"

module cl_axil_reg_access
(
  `include "cl_ports.vh"
);

`include "cl_id_defines.vh"
`include "unused_flr_template.inc"
`include "unused_ddr_template.inc"
`include "unused_cl_sda_template.inc"
`include "unused_apppf_irq_template.inc"
`include "unused_dma_pcis_template.inc"
`include "unused_pcim_template.inc"

  assign cl_sh_id0 = `CL_SH_ID0;
  assign cl_sh_id1 = `CL_SH_ID1;

  //----------------------------  OCL internal signals  ----------------------
  logic         sh_ocl_awvalid_q;
  logic [31:0]  sh_ocl_awaddr_q;
  logic         sh_ocl_wvalid_q;
  logic [31:0]  sh_ocl_wdata_q;
  logic [3:0]   sh_ocl_wstrb_q;
  logic         sh_ocl_bready_q;
  logic         sh_ocl_arvalid_q;
  logic [31:0]  sh_ocl_araddr_q;
  logic         sh_ocl_rready_q;
  logic         ocl_sh_awready_q = 1'b0;
  logic         ocl_sh_wready_q  = 1'b0;
  logic         ocl_sh_bvalid_q  = 1'b0;
  logic [1:0]   ocl_sh_bresp_q   = 2'b00;
  logic         ocl_sh_arready_q = 1'b0;
  logic         ocl_sh_rvalid_q  = 1'b0;
  logic [31:0]  ocl_sh_rdata_q   = 32'h0;
  logic [1:0]   ocl_sh_rresp_q   = 2'b00;

  typedef enum logic [2:0] {
    IDLE=3'd0, WRITE_WAIT=3'd1, WRITE=3'd2, WRITE_RESP=3'd3, READ=3'd4
  } axil_state_t;
  axil_state_t current_state = IDLE, next_state;

  logic data_wr_handshake, addr_rd_handshake, addr_wr_handshake, bresp_handshake, data_rd_handshake;
  logic [31:0] write_addr = 32'h0, read_addr = 32'h0;

  //----------------------------  OCL register slice  ------------------------
  axi_register_slice_light AXIL_OCL_REG_SLC (
    .aclk (clk_main_a0), .aresetn (rst_main_n),
    .s_axi_awaddr (ocl_cl_awaddr), .s_axi_awprot (`AXI_PROT_DEFAULT),
    .s_axi_awvalid(ocl_cl_awvalid), .s_axi_awready(cl_ocl_awready),
    .s_axi_wdata (ocl_cl_wdata), .s_axi_wstrb (ocl_cl_wstrb),
    .s_axi_wvalid(ocl_cl_wvalid), .s_axi_wready(cl_ocl_wready),
    .s_axi_bresp (cl_ocl_bresp), .s_axi_bvalid(cl_ocl_bvalid), .s_axi_bready(ocl_cl_bready),
    .s_axi_araddr (ocl_cl_araddr), .s_axi_arprot (`AXI_PROT_DEFAULT),
    .s_axi_arvalid(ocl_cl_arvalid), .s_axi_arready(cl_ocl_arready),
    .s_axi_rdata (cl_ocl_rdata), .s_axi_rresp (cl_ocl_rresp),
    .s_axi_rvalid(cl_ocl_rvalid), .s_axi_rready(ocl_cl_rready),
    .m_axi_awaddr (sh_ocl_awaddr_q), .m_axi_awprot(), .m_axi_awvalid(sh_ocl_awvalid_q), .m_axi_awready(ocl_sh_awready_q),
    .m_axi_wdata (sh_ocl_wdata_q), .m_axi_wstrb (sh_ocl_wstrb_q), .m_axi_wvalid(sh_ocl_wvalid_q), .m_axi_wready(ocl_sh_wready_q),
    .m_axi_bresp (ocl_sh_bresp_q), .m_axi_bvalid(ocl_sh_bvalid_q), .m_axi_bready(sh_ocl_bready_q),
    .m_axi_araddr (sh_ocl_araddr_q), .m_axi_arvalid(sh_ocl_arvalid_q), .m_axi_arready(ocl_sh_arready_q),
    .m_axi_rdata (ocl_sh_rdata_q), .m_axi_rresp (ocl_sh_rresp_q), .m_axi_rvalid(ocl_sh_rvalid_q), .m_axi_rready(sh_ocl_rready_q)
  );

  //----------------------------  AXI-lite handshake + FSM  ------------------
  always_comb begin
    addr_wr_handshake = sh_ocl_awvalid_q && ocl_sh_awready_q;
    data_wr_handshake = sh_ocl_wvalid_q  && ocl_sh_wready_q;
    bresp_handshake   = ocl_sh_bvalid_q  && sh_ocl_bready_q;
    addr_rd_handshake = sh_ocl_arvalid_q && ocl_sh_arready_q;
    data_rd_handshake = ocl_sh_rvalid_q  && sh_ocl_rready_q;
  end

  always_comb begin
    next_state = current_state;
    case (current_state)
      IDLE:       if (addr_wr_handshake && data_wr_handshake) next_state = WRITE;
                  else if (addr_wr_handshake || data_wr_handshake) next_state = WRITE_WAIT;
                  else if (addr_rd_handshake) next_state = READ;
      WRITE_WAIT: if (addr_wr_handshake || data_wr_handshake) next_state = WRITE;
      WRITE:      next_state = WRITE_RESP;
      WRITE_RESP: if (bresp_handshake) next_state = IDLE;
      READ:       if (data_rd_handshake) next_state = IDLE;
      default:    next_state = IDLE;
    endcase
  end

  always_ff @(posedge clk_main_a0) begin
    if (!rst_main_n) current_state <= IDLE; else current_state <= next_state;
  end

  always_ff @(posedge clk_main_a0) begin
    if (addr_wr_handshake) write_addr <= sh_ocl_awaddr_q;
    if (addr_rd_handshake) read_addr  <= sh_ocl_araddr_q;
  end

  always_ff @(posedge clk_main_a0) begin
    if (!rst_main_n) begin
      ocl_sh_awready_q<=1'b1; ocl_sh_wready_q<=1'b1; ocl_sh_bvalid_q<=1'b0; ocl_sh_bresp_q<=2'b00;
      ocl_sh_arready_q<=1'b1; ocl_sh_rvalid_q<=1'b0; ocl_sh_rresp_q<=2'b00;
    end else begin
      ocl_sh_awready_q <= (next_state==IDLE) || (next_state==WRITE_WAIT);
      ocl_sh_wready_q  <= (next_state==IDLE) || (next_state==WRITE_WAIT);
      ocl_sh_bvalid_q  <= (next_state==WRITE_RESP);
      ocl_sh_bresp_q   <= `AXI_RESP_OKAY;
      ocl_sh_arready_q <= (next_state==IDLE);
      ocl_sh_rvalid_q  <= (next_state==READ);
      ocl_sh_rresp_q   <= `AXI_RESP_OKAY;
    end
  end

  // =========================================================================
  //  BACKTESTER APPLICATION LOGIC
  // =========================================================================
  localparam int NMAX = 1024;                   // tick BRAM depth (matches generic)

  // ---- ParamBlock scalar regs (host-driven; kill_switch tied 0) ----
  logic [31:0] reg_inv_skew   = 32'h0;
  logic [31:0] reg_half_spread= 32'h0;
  logic [31:0] reg_max_inv    = 32'd100;
  logic [31:0] reg_base_qty   = 32'd1;
  logic [15:0] reg_nticks     = 16'h0;

  // ---- tick-mid BRAM (host fills; hw_backtester reads 1-cycle-late) ----
  logic [31:0] tick_ram [0:NMAX-1];             // synchronous read RAM (BRAM)
  logic [9:0]  tick_waddr = 10'h0;              // shared host write / readback index

  // ---- run control ----
  logic        start_r = 1'b0;                  // 1-cycle start pulse
  logic [5:0]  bt_rst_cnt = 6'd63;              // hold backtester reset after config
  wire         bt_rst = (bt_rst_cnt != 6'd0) || !rst_main_n;

  // ---- backtester I/O (to/from the flat wrapper) ----
  wire [15:0] tick_addr;                        // BRAM read address from backtester
  logic [31:0] tick_mid_r = 32'h0;              // registered BRAM read data (1-cyc late)
  logic [31:0] tick_rd_r  = 32'h0;              // host readback port (indexed by tick_waddr)
  wire        bt_done;
  wire [63:0] bt_pnl;
  wire [31:0] bt_fills;

  //---------------------------- WRITE path ----------------------------------
  always_ff @(posedge clk_main_a0) begin
    // default one-cycle pulses
    start_r <= 1'b0;
    if (bt_rst_cnt != 6'd0) bt_rst_cnt <= bt_rst_cnt - 6'd1;

    if (next_state == WRITE) begin
      case (write_addr)
        `ADDR_INV_SKEW    : reg_inv_skew    <= sh_ocl_wdata_q;
        `ADDR_HALF_SPREAD : reg_half_spread <= sh_ocl_wdata_q;
        `ADDR_MAX_INV     : reg_max_inv     <= sh_ocl_wdata_q;
        `ADDR_BASE_QTY    : reg_base_qty    <= sh_ocl_wdata_q;
        `ADDR_NTICKS      : reg_nticks      <= sh_ocl_wdata_q[15:0];
        `ADDR_TICK_WADDR  : tick_waddr      <= sh_ocl_wdata_q[9:0];
        `ADDR_TICK_WDATA  : tick_ram[tick_waddr] <= sh_ocl_wdata_q;
        `ADDR_START       : start_r         <= 1'b1;
        `ADDR_RESET       : bt_rst_cnt      <= 6'd63;
        default           : ;
      endcase
    end
  end

  //---------------------------- tick BRAM read ------------------------------
  // hw_backtester presents tick_addr this cycle; the addressed mid is registered
  // and presented on tick_mid ONE cycle later (synchronous BRAM, tb-accurate).
  always_ff @(posedge clk_main_a0) begin
    tick_mid_r <= tick_ram[tick_addr[9:0]];     // backtester read port (port A)
    tick_rd_r  <= tick_ram[tick_waddr];         // host readback port  (port B)
  end

  //---------------------------- READ path -----------------------------------
  always_ff @(posedge clk_main_a0) begin
    if (addr_rd_handshake) begin
      case (sh_ocl_araddr_q)
        `ADDR_DONE       : ocl_sh_rdata_q <= {31'h0, bt_done};
        `ADDR_PNL_LO     : ocl_sh_rdata_q <= bt_pnl[31:0];
        `ADDR_PNL_HI     : ocl_sh_rdata_q <= bt_pnl[63:32];
        `ADDR_FILLS      : ocl_sh_rdata_q <= bt_fills;
        `ADDR_STATUS     : ocl_sh_rdata_q <= {30'h0, ~bt_done, bt_done};
        `ADDR_NTICKS_RB  : ocl_sh_rdata_q <= {16'h0, reg_nticks};
        `ADDR_TICK_RDATA : ocl_sh_rdata_q <= tick_rd_r;
        `ADDR_INV_SKEW   : ocl_sh_rdata_q <= reg_inv_skew;
        `ADDR_HALF_SPREAD: ocl_sh_rdata_q <= reg_half_spread;
        `ADDR_MAX_INV    : ocl_sh_rdata_q <= reg_max_inv;
        `ADDR_BASE_QTY   : ocl_sh_rdata_q <= reg_base_qty;
        default          : ocl_sh_rdata_q <= `INVALID_ADDR_RESP;
      endcase
    end
  end

  //---------------------------- the hardware backtester ---------------------
  // Flat-port wrapper -> instantiates hw_backtester (VHDL) by name internally.
  hw_backtester_wrap #(.NMAX(NMAX)) BT (
    .clk          (clk_main_a0),
    .rst          (bt_rst),
    .start        (start_r),
    .n_ticks      (reg_nticks),
    .inv_skew_q31 (reg_inv_skew),
    .half_spread  (reg_half_spread),
    .max_inv      (reg_max_inv),
    .base_qty     (reg_base_qty),
    .kill_switch  (1'b0),
    .tick_addr    (tick_addr),
    .tick_mid     (tick_mid_r),
    .done         (bt_done),
    .total_pnl    (bt_pnl),
    .fills        (bt_fills)
  );

  //---------------------------- debug bridge --------------------------------
`ifndef SIMULATION
  cl_debug_bridge CL_DEBUG_BRIDGE (
    .clk(clk_main_a0), .S_BSCAN_drck(drck), .S_BSCAN_shift(shift), .S_BSCAN_tdi(tdi),
    .S_BSCAN_update(update), .S_BSCAN_sel(sel), .S_BSCAN_tdo(tdo), .S_BSCAN_tms(tms),
    .S_BSCAN_tck(tck), .S_BSCAN_runtest(runtest), .S_BSCAN_reset(reset),
    .S_BSCAN_capture(capture), .S_BSCAN_bscanid_en(bscanid_en)
  );
`endif

endmodule
