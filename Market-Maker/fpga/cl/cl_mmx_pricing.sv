// =============================================================================
// Amazon FPGA Hardware Development Kit
//
// Copyright 2026 Amazon.com, Inc. or its affiliates. All Rights Reserved.
//
// Licensed under the Amazon Software License (the "License"). You may not use
// this file except in compliance with the License. A copy of the License is
// located at
//
//    http://aws.amazon.com/asl/
//
// or in the "license" file accompanying this file. This file is distributed on
// an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, express or
// implied. See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================


// CL AXI-LITE REGISTER ACCESS

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

//----------------------------
// Internal signals
//----------------------------

  // Internal OCL signals
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

  // FSM states
  typedef enum logic [2:0] {
    IDLE       = 3'd0,
    WRITE_WAIT = 3'd1,
    WRITE      = 3'd2,
    WRITE_RESP = 3'd3,
    READ       = 3'd4
  } axil_state_t;

  axil_state_t current_state = IDLE;
  axil_state_t next_state;

  // Control and status signals
  logic start_pulse     = 1'b0;
  logic ready_flag      = 1'b0;
  logic sum_read_flag   = 1'b0;
  logic carry_read_flag = 1'b0;

  // Handshake signals
  logic data_wr_handshake;
  logic addr_rd_handshake;
  logic addr_wr_handshake;
  logic bresp_handshake;
  logic data_rd_handshake;

  // Register access logic
  logic [31:0] reg_operand_a      = 32'h0;
  logic [31:0] reg_operand_b      = 32'h0;
  logic [31:0] reg_sum            = 32'h0;
  logic [31:0] reg_carry          = 32'h0;
  logic [31:0] reg_control_status = 32'h0;

  // MMX smart-pricing registers
  logic [31:0] reg_s=0, reg_q=0, reg_inv_skew=0, reg_half_spread=0,
               reg_max_inv=0, reg_base_qty=0, reg_skews=0, reg_kill=0,
               reg_bid=0, reg_ask=0, reg_qty_o=0, reg_cancel=0;
  logic [31:0] reg_imbalance=0, reg_obi_gain=0, reg_k_shift=6, reg_vol_gain=0,
               reg_pnl=0, reg_cap_thresh=5, reg_pnl_drop=0;
  logic [31:0] reg_latency=0;
  logic        tick_strobe=0, host_reset_p=0;

  // Address capture
  logic [31:0] write_addr = 32'h0;
  logic [31:0] read_addr  = 32'h0;

//----------------------------
// OCL Interface Connections
//----------------------------

// OCL register slice
axi_register_slice_light AXIL_OCL_REG_SLC (
  .aclk          (clk_main_a0),
  .aresetn       (rst_main_n),
  .s_axi_awaddr  (ocl_cl_awaddr),
  .s_axi_awprot  (`AXI_PROT_DEFAULT),
  .s_axi_awvalid (ocl_cl_awvalid),
  .s_axi_awready (cl_ocl_awready),
  .s_axi_wdata   (ocl_cl_wdata),
  .s_axi_wstrb   (ocl_cl_wstrb),
  .s_axi_wvalid  (ocl_cl_wvalid),
  .s_axi_wready  (cl_ocl_wready),
  .s_axi_bresp   (cl_ocl_bresp),
  .s_axi_bvalid  (cl_ocl_bvalid),
  .s_axi_bready  (ocl_cl_bready),
  .s_axi_araddr  (ocl_cl_araddr),
  .s_axi_arprot  (`AXI_PROT_DEFAULT),
  .s_axi_arvalid (ocl_cl_arvalid),
  .s_axi_arready (cl_ocl_arready),
  .s_axi_rdata   (cl_ocl_rdata),
  .s_axi_rresp   (cl_ocl_rresp),
  .s_axi_rvalid  (cl_ocl_rvalid),
  .s_axi_rready  (ocl_cl_rready),
  .m_axi_awaddr  (sh_ocl_awaddr_q),
  .m_axi_awprot  (),
  .m_axi_awvalid (sh_ocl_awvalid_q),
  .m_axi_awready (ocl_sh_awready_q),
  .m_axi_wdata   (sh_ocl_wdata_q),
  .m_axi_wstrb   (sh_ocl_wstrb_q),
  .m_axi_wvalid  (sh_ocl_wvalid_q),
  .m_axi_wready  (ocl_sh_wready_q),
  .m_axi_bresp   (ocl_sh_bresp_q),
  .m_axi_bvalid  (ocl_sh_bvalid_q),
  .m_axi_bready  (sh_ocl_bready_q),
  .m_axi_araddr  (sh_ocl_araddr_q),
  .m_axi_arvalid (sh_ocl_arvalid_q),
  .m_axi_arready (ocl_sh_arready_q),
  .m_axi_rdata   (ocl_sh_rdata_q),
  .m_axi_rresp   (ocl_sh_rresp_q),
  .m_axi_rvalid  (ocl_sh_rvalid_q),
  .m_axi_rready  (sh_ocl_rready_q)
);

//-------------------------------------------------
// AXI-lite Handshake & FSM
//-------------------------------------------------

// Handshake signals
always_comb begin
  addr_wr_handshake = sh_ocl_awvalid_q && ocl_sh_awready_q;
  data_wr_handshake = sh_ocl_wvalid_q  && ocl_sh_wready_q;
  bresp_handshake   = ocl_sh_bvalid_q  && sh_ocl_bready_q;
  addr_rd_handshake = sh_ocl_arvalid_q && ocl_sh_arready_q;
  data_rd_handshake = ocl_sh_rvalid_q  && sh_ocl_rready_q;
end

// FSM next state logic
always_comb begin
  next_state = current_state;
  case (current_state)

    IDLE: begin
      if (addr_wr_handshake && data_wr_handshake)
        next_state = WRITE;
      else if (addr_wr_handshake || data_wr_handshake)
        next_state = WRITE_WAIT;
      else if (addr_rd_handshake)
        next_state = READ;
    end

    WRITE_WAIT: begin
      if (addr_wr_handshake || data_wr_handshake)
        next_state = WRITE;
    end

    WRITE: begin
      next_state = WRITE_RESP;
    end

    WRITE_RESP: begin
      if (bresp_handshake)
        next_state = IDLE;
    end

    READ: begin
      if (data_rd_handshake)
        next_state = IDLE;
    end

    default: begin
      next_state = IDLE;
    end
  endcase
end

// FSM state register
always_ff @(posedge clk_main_a0) begin
  if (!rst_main_n)
    current_state <= IDLE;
  else
    current_state <= next_state;
end

//============================================
// AXI-lite CONTROL PATH
//============================================

// Address capture
always_ff @(posedge clk_main_a0) begin
  if (addr_wr_handshake)
    write_addr <= sh_ocl_awaddr_q;

  if (addr_rd_handshake)
    read_addr <= sh_ocl_araddr_q;
end

// AXI-Lite output signals
always_ff @(posedge clk_main_a0) begin
  if (!rst_main_n) begin
    ocl_sh_awready_q <= 1'b1;
    ocl_sh_wready_q  <= 1'b1;
    ocl_sh_bvalid_q  <= 1'b0;
    ocl_sh_bresp_q   <= 2'b00;
    ocl_sh_arready_q <= 1'b1;
    ocl_sh_rvalid_q  <= 1'b0;
    ocl_sh_rresp_q   <= 2'b00;
  end
  else begin
    ocl_sh_awready_q <= (next_state == IDLE) || (next_state == WRITE_WAIT);
    ocl_sh_wready_q  <= (next_state == IDLE) || (next_state == WRITE_WAIT);
    ocl_sh_bvalid_q  <= (next_state == WRITE_RESP);
    ocl_sh_bresp_q   <= `AXI_RESP_OKAY;
    ocl_sh_arready_q <= (next_state == IDLE);
    ocl_sh_rvalid_q  <= (next_state == READ);
    ocl_sh_rresp_q   <= `AXI_RESP_OKAY;
  end
end

//============================================
// AXI-lite DATA PATH
//============================================

// Register writes
always_ff @(posedge clk_main_a0) begin
  tick_strobe <= 1'b0; host_reset_p <= 1'b0;
  if (next_state == WRITE) begin
    case (write_addr)
      `ADDR_S           : begin reg_s <= sh_ocl_wdata_q; tick_strobe <= 1'b1; end
      `ADDR_Q           : reg_q           <= sh_ocl_wdata_q;
      `ADDR_INV_SKEW    : reg_inv_skew    <= sh_ocl_wdata_q;
      `ADDR_HALF_SPREAD : reg_half_spread <= sh_ocl_wdata_q;
      `ADDR_MAX_INV     : reg_max_inv     <= sh_ocl_wdata_q;
      `ADDR_BASE_QTY    : reg_base_qty    <= sh_ocl_wdata_q;
      `ADDR_SKEWS       : reg_skews       <= sh_ocl_wdata_q;
      `ADDR_KILL        : reg_kill        <= sh_ocl_wdata_q;
      `ADDR_IMBALANCE   : reg_imbalance   <= sh_ocl_wdata_q;
      `ADDR_OBI_GAIN    : reg_obi_gain    <= sh_ocl_wdata_q;
      `ADDR_K_SHIFT     : reg_k_shift     <= sh_ocl_wdata_q;
      `ADDR_VOL_GAIN    : reg_vol_gain    <= sh_ocl_wdata_q;
      `ADDR_PNL         : reg_pnl         <= sh_ocl_wdata_q;
      `ADDR_CAP_THRESH  : reg_cap_thresh  <= sh_ocl_wdata_q;
      `ADDR_PNL_DROP    : reg_pnl_drop    <= sh_ocl_wdata_q;
      `ADDR_HOST_RESET  : host_reset_p    <= 1'b1;
    endcase
  end
end

// Register reads
always_ff @(posedge clk_main_a0) begin
  if (addr_rd_handshake) begin
    case (sh_ocl_araddr_q)
      `ADDR_S           : ocl_sh_rdata_q <= reg_s;
      `ADDR_Q           : ocl_sh_rdata_q <= reg_q;
      `ADDR_BID         : ocl_sh_rdata_q <= reg_bid;
      `ADDR_ASK         : ocl_sh_rdata_q <= reg_ask;
      `ADDR_QTY         : ocl_sh_rdata_q <= reg_qty_o;
      `ADDR_CANCEL      : ocl_sh_rdata_q <= reg_cancel;
      `ADDR_SPREAD_ADD  : ocl_sh_rdata_q <= spread_add_r;
      `ADDR_VOL_OUT     : ocl_sh_rdata_q <= ewma_vol[47:16];
      `ADDR_HARD_KILL   : ocl_sh_rdata_q <= {31'h0, hk};
      `ADDR_TRIP        : ocl_sh_rdata_q <= {30'h0, trip};
      `ADDR_LATENCY     : ocl_sh_rdata_q <= reg_latency;
      `ADDR_CONTROL_STATUS : ocl_sh_rdata_q <= reg_control_status;
      default           : ocl_sh_rdata_q <= `INVALID_ADDR_RESP;
    endcase
  end
end

//============================================
// APPLICATION LOGIC
//============================================

// Start pulse generation
always_ff @(posedge clk_main_a0) begin
  start_pulse <= 1'b0;
  if (current_state == WRITE)
    start_pulse <= (write_addr == `ADDR_CONTROL_STATUS) && sh_ocl_wdata_q[0];
end

// =========================== MMX SMART PRICING ===========================
function automatic logic signed [31:0] sat32(input logic signed [63:0] v);
  if (v > 64'sd2147483647)       sat32 = 32'sd2147483647;
  else if (v < -64'sd2147483648) sat32 = -32'sd2147483648;
  else                           sat32 = v[31:0];
endfunction

// ---- Order-Book Imbalance skew (obi_skew.vhd mirror): (imb*gain) sra 15, clamp +/-64
// Registered in its own stage so the 16x16 multiply stays off stage-1's critical path.
logic signed [31:0] obi_skew_adj = 0;
always_ff @(posedge clk_main_a0) begin
  logic signed [31:0] p; logic signed [63:0] sh;
  p  = $signed(reg_imbalance[15:0]) * $signed(reg_obi_gain[15:0]);
  sh = $signed({{32{p[31]}}, p}) >>> 15;
  if      (sh >  64) obi_skew_adj <= 32'sd64;
  else if (sh < -64) obi_skew_adj <= -32'sd64;
  else               obi_skew_adj <= sh[31:0];
end

// ---- Volatility EWMA -> spread_add, PIPELINED for 250 MHz (fixed decay lambda=1-2^-6).
// One op per stage (no chained mult+barrel-shift+mult); fixed shift = free routing.
localparam int VKSH = 6;
logic signed [31:0] prev_mid = 0;
logic signed [33:0] vd = 0;   logic vd_v = 0;   // V1: delta
logic [63:0]        vd2 = 0;   logic vd2_v = 0;  // V2: square (DSP)
logic [63:0]        ewma_vol = 0;                // V3: EWMA (fixed shifts)
logic [31:0]        spread_add_r = 0;            // V4: gain + clamp (DSP)
always_ff @(posedge clk_main_a0) begin
  if (!rst_main_n) begin prev_mid<=0; vd<=0; vd_v<=0; vd2<=0; vd2_v<=0; ewma_vol<=0; spread_add_r<=0; end
  else begin
    if (tick_strobe) begin vd <= $signed(reg_s) - $signed(prev_mid); prev_mid <= reg_s; vd_v <= 1'b1; end
    else vd_v <= 1'b0;
    vd2 <= $unsigned(vd*vd); vd2_v <= vd_v;
    if (vd2_v) ewma_vol <= ewma_vol - (ewma_vol >> VKSH) + (vd2 >> VKSH);
    begin logic [63:0] vsp; vsp = (ewma_vol * reg_vol_gain[15:0]) >> 16;
      spread_add_r <= (vsp > 64'd65535) ? 32'd65535 : vsp[31:0]; end
  end
end

// ---- Hardware circuit breaker (circuit_breaker_hw.vhd mirror, simplified count-since-reset)
logic        cap_prev=0, hk=0; logic [7:0] cap_cnt=0; logic [1:0] trip=0;
always_ff @(posedge clk_main_a0) begin
  logic cap_now; logic signed [31:0] absq;
  absq  = ($signed(reg_q) < 0) ? -$signed(reg_q) : $signed(reg_q);
  cap_now = (absq >= $signed({1'b0, reg_max_inv[30:0]}));
  if (!rst_main_n || host_reset_p) begin hk<=0; cap_cnt<=0; trip<=0; cap_prev<=cap_now; end
  else begin
    if (cap_now && !cap_prev) cap_cnt <= cap_cnt + 8'd1;   // rising-edge breach
    cap_prev <= cap_now;
    if (!hk) begin
      if ((cap_cnt + (cap_now && !cap_prev ? 8'd1:8'd0)) >= reg_cap_thresh[7:0]) begin hk<=1; trip<=2'b01; end
      else if ($signed(reg_pnl) < -$signed(reg_pnl_drop)) begin hk<=1; trip<=2'b10; end
    end
  end
end

// ---- latency counter: tag through the 4-stage pipeline
logic tag1=0,tag2=0,tag3=0,tag4=0; logic [7:0] latc=0; logic measuring=0;
always_ff @(posedge clk_main_a0) begin
  tag1 <= tick_strobe;            // inject on new tick
  tag2 <= tag1; tag3 <= tag2; tag4 <= tag3;
  if (tick_strobe) begin latc <= 8'd1; measuring <= 1'b1; end
  else if (measuring) begin
    latc <= latc + 8'd1;
    if (tag4) begin reg_latency <= {24'h0, latc}; measuring <= 1'b0; end
  end
end

// ---- 4-stage pricing pipeline (v2) with OBI + vol folded in ----
logic signed [63:0] qc_s1=0, is_s1=0;
logic signed [31:0] s_s1=0, hs_s1=0;  logic signed [15:0] bsk_s1=0, ask_s1=0;
logic [31:0] bq_s1=0; logic kill_s1=0, kcap_s1=0;
logic signed [63:0] prod_s2=0;
logic signed [31:0] s_s2=0, hs_s2=0;  logic signed [15:0] bsk_s2=0, ask_s2=0;
logic [31:0] bq_s2=0; logic kill_s2=0, kcap_s2=0;
logic signed [63:0] r_s3=0;
logic signed [31:0] hs_s3=0;  logic signed [15:0] bsk_s3=0, ask_s3=0;
logic [31:0] bq_s3=0; logic kill_s3=0, kcap_s3=0;

always_ff @(posedge clk_main_a0) begin
  // stage 1: clamp inventory; fold OBI skew into the mid (s + obi_skew_adj);
  //          fold vol widener into half_spread (half_spread + spread_add)
  begin
    logic signed [63:0] cap, qv, qcl;
    cap = $signed({1'b0, reg_max_inv});
    qv  = $signed(reg_q);
    qcl = qv;
    if (qcl > cap)  qcl = cap;
    if (qcl < -cap) qcl = -cap;
    qc_s1 <= qcl;
    kcap_s1 <= (qv > cap) || (qv < -cap);
    is_s1 <= $signed(reg_inv_skew);
    s_s1  <= $signed(reg_s) + obi_skew_adj;
    hs_s1 <= $signed(reg_half_spread) + $signed({1'b0, spread_add_r});
    bsk_s1<=$signed(reg_skews[15:0]); ask_s1<=$signed(reg_skews[31:16]);
    bq_s1<=reg_base_qty; kill_s1<=reg_kill[0];
  end
  // stage 2: multiply
  prod_s2 <= qc_s1 * is_s1;
  s_s2<=s_s1; hs_s2<=hs_s1; bsk_s2<=bsk_s1; ask_s2<=ask_s1;
  bq_s2<=bq_s1; kill_s2<=kill_s1; kcap_s2<=kcap_s1;
  // stage 3: skew + reservation price
  r_s3 <= $signed(s_s2) - ((prod_s2 + 64'sd1073741824) >>> 31);
  hs_s3<=hs_s2; bsk_s3<=bsk_s2; ask_s3<=ask_s2;
  bq_s3<=bq_s2; kill_s3<=kill_s2; kcap_s3<=kcap_s2;
  // stage 4: bid/ask + saturate ; cancel = kill | cap | hard_kill
  if (!rst_main_n) begin
    reg_bid<=0; reg_ask<=0; reg_qty_o<=0; reg_cancel<=0;
  end else begin
    reg_bid    <= sat32(r_s3 - $signed(hs_s3) - $signed(bsk_s3));
    reg_ask    <= sat32(r_s3 + $signed(hs_s3) + $signed(ask_s3));
    reg_qty_o  <= bq_s3;
    reg_cancel <= {31'h0, (kill_s3 | kcap_s3 | hk)};
  end
end

// Ready flag
always_ff @(posedge clk_main_a0) begin
  if (start_pulse)
    ready_flag <= 1'b1;
  else if (sum_read_flag && carry_read_flag)
    ready_flag <= 1'b0;
end

// Read flags
always_ff @(posedge clk_main_a0) begin
  if (start_pulse) begin
    sum_read_flag   <= 1'b0;
    carry_read_flag <= 1'b0;
  end
  else if (addr_rd_handshake) begin
    if (sh_ocl_araddr_q == `ADDR_BID)
      sum_read_flag <= 1'b1;
    if (sh_ocl_araddr_q == `ADDR_ASK)
      carry_read_flag <= 1'b1;
  end
end

// Control/status register
always_ff @(posedge clk_main_a0) begin
  if (!rst_main_n)
    reg_control_status <= 32'h0;
  else
    reg_control_status <= {30'h0, ready_flag, start_pulse};
end

//-------------------------------------
// Debug Bridge
//-------------------------------------

`ifndef SIMULATION
cl_debug_bridge CL_DEBUG_BRIDGE (
  .clk                (clk_main_a0),
  .S_BSCAN_drck       (drck),
  .S_BSCAN_shift      (shift),
  .S_BSCAN_tdi        (tdi),
  .S_BSCAN_update     (update),
  .S_BSCAN_sel        (sel),
  .S_BSCAN_tdo        (tdo),
  .S_BSCAN_tms        (tms),
  .S_BSCAN_tck        (tck),
  .S_BSCAN_runtest    (runtest),
  .S_BSCAN_reset      (reset),
  .S_BSCAN_capture    (capture),
  .S_BSCAN_bscanid_en (bscanid_en)
);
`endif // SIMULATION

`ifndef NO_CL_AXIL_DEBUG_ILA

ila_axil OCL_AXIL_ILA (
  .clk     (clk_main_a0),
  .probe0  (sh_ocl_awaddr_q),
  .probe1  (sh_ocl_awvalid_q),
  .probe2  (ocl_sh_awready_q),
  .probe3  (sh_ocl_wdata_q),
  .probe4  (sh_ocl_wstrb_q),
  .probe5  (sh_ocl_wvalid_q),
  .probe6  (ocl_sh_wready_q),
  .probe7  (ocl_sh_bresp_q),
  .probe8  (ocl_sh_bvalid_q),
  .probe9  (sh_ocl_bready_q),
  .probe10 (sh_ocl_araddr_q),
  .probe11 (sh_ocl_arvalid_q),
  .probe12 (ocl_sh_arready_q),
  .probe13 (ocl_sh_rdata_q),
  .probe14 (ocl_sh_rresp_q),
  .probe15 (ocl_sh_rvalid_q),
  .probe16 (sh_ocl_rready_q)
);

`endif // NO_CL_AXIL_DEBUG_ILA

endmodule
