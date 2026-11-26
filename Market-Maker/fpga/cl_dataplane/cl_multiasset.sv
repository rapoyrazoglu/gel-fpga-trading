// =============================================================================
// cl_multiasset.sv  —  MULTI-ASSET PARALLEL PRICING on AWS F2 (VU47P)
//
// Puts pricing_engine_multi (fpga/rtl/pricing_engine_multi.vhd) on silicon: N
// asset lanes priced IN THE SAME CYCLE from one shared strobe. Each lane has its
// own (s, q, ParamBlock) input registers and its own (bid, ask, qty, cancel)
// read registers. The host pokes every lane's inputs, pulses ADDR_STROBE
// (valid_in), and peeks each lane's quote back — proving all N lanes compute a
// bit-exact quote in parallel on real hardware.
//
// The OCL AXI-Lite plumbing (register slice + FSM + handshake + address capture
// + AXI output signals + shell includes + cl_debug_bridge) is REUSED VERBATIM
// from fpga/cl_dataplane/cl_dataplane.sv. Only the APPLICATION logic (the data-
// plane instance + its register decode) is replaced with the multi-asset lanes.
//
// CROSS-LANGUAGE NOTE: pricing_engine_multi has VHDL record/array ports
// (tick_vec_t / pb_vec_t / quote_vec_t from work.mmx_multi_pkg), which SV cannot
// connect to directly. We instantiate the thin VHDL flattening wrapper
// pricing_engine_multi_flat (fpga/cl_dataplane/pricing_engine_multi_flat.vhd),
// whose ports are flat std_logic_vectors; that wrapper builds the records and
// instantiates pricing_engine_multi BY NAME. Build order (read_vhdl -vhdl2008):
//   mmx_pkg -> mmx_multi_pkg -> pricing_engine_beh -> pricing_engine_multi
//   -> pricing_engine_multi_flat
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
  //  MULTI-ASSET APPLICATION LOGIC
  // =========================================================================
  localparam int N = `MA_N_LANES;             // must match pricing_engine_multi_flat generic N

  // ---- per-lane INPUT registers (host-written) ----
  logic [31:0] reg_s          [0:N-1];
  logic [31:0] reg_q          [0:N-1];
  logic [31:0] reg_inv_skew   [0:N-1];
  logic [31:0] reg_half_spread[0:N-1];
  logic [31:0] reg_max_inv    [0:N-1];
  logic [31:0] reg_base_qty   [0:N-1];
  logic [31:0] reg_skews      [0:N-1];        // [15:0]=bid_skew, [31:16]=ask_skew
  logic [31:0] reg_kill       [0:N-1];        // [0]=kill_switch

  // ---- per-lane OUTPUT registers (host-read, latched on quote_valid) ----
  logic [31:0] reg_bid   [0:N-1];
  logic [31:0] reg_ask   [0:N-1];
  logic [31:0] reg_qty   [0:N-1];
  logic [31:0] reg_cancel[0:N-1];

  // ---- shared strobe + reset + status ----
  logic       valid_pulse  = 1'b0;            // one-cycle valid_in to all lanes
  logic       result_ready = 1'b0;            // sticky: set on quote_valid, cleared on strobe
  logic [4:0] dp_rst_cnt   = 5'd31;           // hold reset after config
  wire        dp_rst = (dp_rst_cnt != 5'd0) || !rst_main_n;

  // ---- flattened lane buses to/from the VHDL wrapper ----
  logic [N*32-1:0] s_flat, q_flat, is_flat, hs_flat, mx_flat, bq_flat;
  logic [N*16-1:0] bsk_flat, ask_flat;
  logic [N-1:0]    kill_flat;
  logic [N*32-1:0] bid_flat_o, ask_flat_o, qty_flat_o;
  logic [N-1:0]    cancel_flat_o;
  logic            quote_valid_live;

  genvar gi;
  generate
    for (gi = 0; gi < N; gi = gi + 1) begin : PACK
      assign s_flat [gi*32 +: 32] = reg_s          [gi];
      assign q_flat [gi*32 +: 32] = reg_q          [gi];
      assign is_flat[gi*32 +: 32] = reg_inv_skew   [gi];
      assign hs_flat[gi*32 +: 32] = reg_half_spread[gi];
      assign mx_flat[gi*32 +: 32] = reg_max_inv    [gi];
      assign bq_flat[gi*32 +: 32] = reg_base_qty   [gi];
      assign bsk_flat[gi*16 +: 16] = reg_skews[gi][15:0];
      assign ask_flat[gi*16 +: 16] = reg_skews[gi][31:16];
      assign kill_flat[gi]         = reg_kill[gi][0];
    end
  endgenerate

  //---------------------------- WRITE path ----------------------------------
  // Address layout (byte addr): input region 0x00..0x7F, lane = addr[6:5],
  // word = addr[4:2]. Control region: ADDR_STROBE / ADDR_RESET (bit8 = 0).
  integer wi;
  always_ff @(posedge clk_main_a0) begin
    valid_pulse <= 1'b0;                       // default one-cycle pulse
    if (dp_rst_cnt != 5'd0) dp_rst_cnt <= dp_rst_cnt - 5'd1;

    if (next_state == WRITE) begin
      if (write_addr < 32'h80) begin           // ---- per-lane input registers ----
        case (write_addr[4:2])
          3'd0: reg_s          [write_addr[6:5]] <= sh_ocl_wdata_q;
          3'd1: reg_q          [write_addr[6:5]] <= sh_ocl_wdata_q;
          3'd2: reg_inv_skew   [write_addr[6:5]] <= sh_ocl_wdata_q;
          3'd3: reg_half_spread[write_addr[6:5]] <= sh_ocl_wdata_q;
          3'd4: reg_max_inv    [write_addr[6:5]] <= sh_ocl_wdata_q;
          3'd5: reg_base_qty   [write_addr[6:5]] <= sh_ocl_wdata_q;
          3'd6: reg_skews      [write_addr[6:5]] <= sh_ocl_wdata_q;
          3'd7: reg_kill       [write_addr[6:5]] <= sh_ocl_wdata_q;
        endcase
      end else begin                            // ---- control registers ----
        case (write_addr)
          `ADDR_STROBE : valid_pulse <= 1'b1;   // pulse valid_in to all lanes
          `ADDR_RESET  : dp_rst_cnt  <= 5'd31;
          default      : ;
        endcase
      end
    end
  end

  //---------------------------- result capture ------------------------------
  // Outputs are registered inside each pricing_engine_beh lane; latch them on
  // quote_valid so host peeks (arriving many us later) read stable data.
  always_ff @(posedge clk_main_a0) begin
    if (dp_rst) begin
      result_ready <= 1'b0;
      for (wi = 0; wi < N; wi = wi + 1) begin
        reg_bid[wi] <= 32'h0; reg_ask[wi] <= 32'h0;
        reg_qty[wi] <= 32'h0; reg_cancel[wi] <= 32'h0;
      end
    end else begin
      if (valid_pulse) result_ready <= 1'b0;    // new request pending
      if (quote_valid_live) begin
        result_ready <= 1'b1;
        for (wi = 0; wi < N; wi = wi + 1) begin
          reg_bid   [wi] <= bid_flat_o[wi*32 +: 32];
          reg_ask   [wi] <= ask_flat_o[wi*32 +: 32];
          reg_qty   [wi] <= qty_flat_o[wi*32 +: 32];
          reg_cancel[wi] <= {31'h0, cancel_flat_o[wi]};
        end
      end
    end
  end

  //---------------------------- READ path -----------------------------------
  // Output region 0x100..0x13F (bit8 = 1): lane = addr[5:4], word = addr[3:2].
  // Control reads (bit8 = 0): ADDR_STATUS.
  always_ff @(posedge clk_main_a0) begin
    if (addr_rd_handshake) begin
      if (sh_ocl_araddr_q[8]) begin             // ---- per-lane outputs ----
        case (sh_ocl_araddr_q[3:2])
          2'd0: ocl_sh_rdata_q <= reg_bid   [sh_ocl_araddr_q[5:4]];
          2'd1: ocl_sh_rdata_q <= reg_ask   [sh_ocl_araddr_q[5:4]];
          2'd2: ocl_sh_rdata_q <= reg_qty   [sh_ocl_araddr_q[5:4]];
          2'd3: ocl_sh_rdata_q <= reg_cancel[sh_ocl_araddr_q[5:4]];
        endcase
      end else begin                            // ---- control / status ----
        case (sh_ocl_araddr_q)
          `ADDR_STATUS : ocl_sh_rdata_q <= {30'h0, quote_valid_live, result_ready};
          `ADDR_N_LANES: ocl_sh_rdata_q <= N;
          default      : ocl_sh_rdata_q <= `INVALID_ADDR_RESP;
        endcase
      end
    end
  end

  //---------------------------- the multi-asset pricing core ----------------
  // VHDL flattening wrapper: builds tick_vec_t/pb_vec_t/quote_vec_t and
  // instantiates pricing_engine_multi (work.mmx_multi_pkg) by name. Its generic
  // N defaults to 4 and MUST equal `MA_N_LANES above.
  pricing_engine_multi_flat U_PRICE (
    .clk            (clk_main_a0),
    .rst            (dp_rst),
    .valid_in       (valid_pulse),
    .s_in           (s_flat),
    .q_in           (q_flat),
    .pb_inv_skew    (is_flat),
    .pb_half_spread (hs_flat),
    .pb_max_inv     (mx_flat),
    .pb_base_qty    (bq_flat),
    .pb_bid_skew    (bsk_flat),
    .pb_ask_skew    (ask_flat),
    .pb_kill        (kill_flat),
    .bid            (bid_flat_o),
    .ask            (ask_flat_o),
    .qty            (qty_flat_o),
    .cancel         (cancel_flat_o),
    .quote_valid    (quote_valid_live)
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
