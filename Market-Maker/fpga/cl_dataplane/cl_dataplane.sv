// =============================================================================
// cl_axil_reg_access.sv  —  FULL GEL DATA PLANE on AWS F2 (VU47P)
//
// Wraps the mmx_dataplane VHDL pipeline
//   mac_udp_rx -> feed_parser -> order_book -> pricing_engine -> quote_decider
//   -> order_encoder (OUCH), plus param_block_loader (HMAC-verified auto params)
// behind the shell OCL AXI-Lite BAR. The host:
//   1. writes the 16-word signed ParamBlock (auto-loaded + HMAC-checked on chip),
//   2. loads a market-data Ethernet/UDP/ITCH frame into the RX replay buffer,
//   3. pulses RX_GO -> the frame streams into the pipeline at one beat/cycle,
//   4. reads the emitted OUCH order frame out of the TX buffer,
//   5. reads ADDR_LATENCY = data-in -> order-out cycles (wire-to-wire latency).
//
// The OCL AXI-Lite plumbing (register slice + FSM + handshake) is reused verbatim
// from the AWS cl_axil_reg_access example; only the application logic changed.
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
  //  DATA-PLANE BRIDGE
  // =========================================================================
  localparam int NB = 32;                       // max beats per frame (frames are ~8)

  // ---- ParamBlock write path (-> param_block_loader) ----
  logic [3:0]  pb_addr   = 4'h0;
  logic [31:0] s_wr_data = 32'h0;
  logic        s_wr_en   = 1'b0, s_commit = 1'b0;

  // ---- RX replay buffer (distributed RAM: combinational read) ----
  logic [63:0] rx_data [0:NB-1];
  logic [8:0]  rx_kl   [0:NB-1];                // [7:0]=tkeep [8]=tlast
  logic [4:0]  rx_waddr  = 5'h0;
  logic [31:0] rx_lo     = 32'h0;
  logic [5:0]  rx_nbeats = 6'd8;

  // ---- TX capture buffer ----
  logic [63:0] tx_data [0:NB-1];
  logic [8:0]  tx_kl   [0:NB-1];
  logic [5:0]  tx_wr_ptr = 6'h0;

  // ---- data-plane reset (active-high; hold ~16 cyc on a host reset) ----
  logic [4:0] dp_rst_cnt = 5'd31;               // start held after config
  wire        dp_rst = (dp_rst_cnt != 5'd0) || !rst_main_n;

  // ---- config / status ----
  logic [31:0] reg_session = 32'hDEADBEEF, reg_udp = 32'h00001F90;
  logic [31:0] reg_latency = 32'h0;
  logic        replay_busy = 1'b0, tx_done = 1'b0;
  logic        rx_go_r = 1'b0;                   // registered RX_GO strobe (1 cyc)
  logic        book_clr_r = 1'b0;                // registered book-clear strobe (1 cyc)

  // ---- data-plane AXIS wires ----
  logic [63:0] mac_tdata; logic [7:0] mac_tkeep; logic mac_tvalid, mac_tlast, mac_tready;
  logic [63:0] m_tdata;   logic [7:0] m_tkeep;   logic m_tvalid,   m_tlast;
  wire         m_tready = (tx_wr_ptr < NB-1);    // accept until buffer near-full
  logic [31:0] q_cpu_reg; logic [1:0] load_err;
  logic [31:0] dbg_qc;

  // ---- latency timer: free-run from RX_GO, latch on the first TX beat ----
  logic [31:0] lat_cnt = 32'h0; logic lat_run = 1'b0;

  //---------------------------- WRITE path ----------------------------------
  always_ff @(posedge clk_main_a0) begin
    // default one-cycle pulses
    s_wr_en <= 1'b0; s_commit <= 1'b0; rx_go_r <= 1'b0; book_clr_r <= 1'b0;
    if (dp_rst_cnt != 5'd0) dp_rst_cnt <= dp_rst_cnt - 5'd1;

    if (next_state == WRITE) begin
      case (write_addr)
        `ADDR_PB_ADDR   : pb_addr   <= sh_ocl_wdata_q[3:0];
        `ADDR_PB_DATA   : begin s_wr_data <= sh_ocl_wdata_q; s_wr_en <= 1'b1; end
        `ADDR_PB_COMMIT : s_commit  <= 1'b1;
        `ADDR_RX_WADDR  : rx_waddr  <= sh_ocl_wdata_q[4:0];
        `ADDR_RX_DATA_LO: rx_lo     <= sh_ocl_wdata_q;
        `ADDR_RX_DATA_HI: rx_data[rx_waddr] <= {sh_ocl_wdata_q, rx_lo};
        `ADDR_RX_KEEP   : rx_kl[rx_waddr]   <= sh_ocl_wdata_q[8:0];
        `ADDR_RX_NBEATS : rx_nbeats <= sh_ocl_wdata_q[5:0];
        `ADDR_RX_GO     : rx_go_r  <= 1'b1;
        `ADDR_SESSION   : reg_session <= sh_ocl_wdata_q;
        `ADDR_UDP_PORT  : reg_udp     <= sh_ocl_wdata_q;
        `ADDR_RESET     : dp_rst_cnt  <= 5'd31;
        `ADDR_BOOK_CLEAR: book_clr_r  <= 1'b1;
        default         : ;
      endcase
    end
  end

  //---------------------------- RX replay FSM -------------------------------
  logic [5:0] rp_idx = 6'h0;
  assign mac_tdata  = rx_data[rp_idx[4:0]];
  assign mac_tkeep  = rx_kl  [rp_idx[4:0]][7:0];
  assign mac_tlast  = rx_kl  [rp_idx[4:0]][8];
  assign mac_tvalid = replay_busy;

  always_ff @(posedge clk_main_a0) begin
    if (dp_rst) begin replay_busy <= 1'b0; rp_idx <= 6'h0; end
    else begin
      if (rx_go_r && !replay_busy) begin replay_busy <= 1'b1; rp_idx <= 6'h0; end
      else if (replay_busy && mac_tvalid && mac_tready) begin
        if (rp_idx == rx_nbeats-1) begin replay_busy <= 1'b0; rp_idx <= 6'h0; end
        else rp_idx <= rp_idx + 6'd1;
      end
    end
  end

  //---------------------------- TX capture ----------------------------------
  // Reset the capture pointer on every RX_GO so each replayed frame writes its
  // OWN emitted order from beat 0 (enables a continuous multi-message feed where
  // the host reads the fresh quote after each fire; book/inventory persist).
  always_ff @(posedge clk_main_a0) begin
    if (dp_rst)        begin tx_wr_ptr <= 6'h0; tx_done <= 1'b0; end
    else if (rx_go_r)  begin tx_wr_ptr <= 6'h0; tx_done <= 1'b0; end
    else if (m_tvalid && m_tready) begin
      tx_data[tx_wr_ptr[4:0]] <= m_tdata;
      tx_kl  [tx_wr_ptr[4:0]] <= {m_tlast, m_tkeep};
      tx_wr_ptr <= tx_wr_ptr + 6'd1;
      if (m_tlast) tx_done <= 1'b1;
    end
  end

  //---------------------------- latency timer -------------------------------
  always_ff @(posedge clk_main_a0) begin
    if (dp_rst) begin lat_cnt <= 32'h0; lat_run <= 1'b0; reg_latency <= 32'h0; end
    else begin
      if (rx_go_r) begin lat_run <= 1'b1; lat_cnt <= 32'h0; end
      else if (lat_run) begin
        lat_cnt <= lat_cnt + 32'd1;
        if (m_tvalid && m_tready) begin reg_latency <= lat_cnt + 32'd1; lat_run <= 1'b0; end
      end
    end
  end

  //---------------------------- READ path -----------------------------------
  always_ff @(posedge clk_main_a0) begin
    if (addr_rd_handshake) begin
      case (sh_ocl_araddr_q)
        `ADDR_TX_COUNT  : ocl_sh_rdata_q <= {26'h0, tx_wr_ptr};
        `ADDR_TX_RADDR  : ocl_sh_rdata_q <= {27'h0, rx_waddr};
        `ADDR_TX_DATA_LO: ocl_sh_rdata_q <= tx_data[rx_waddr][31:0];
        `ADDR_TX_DATA_HI: ocl_sh_rdata_q <= tx_data[rx_waddr][63:32];
        `ADDR_TX_KEEP   : ocl_sh_rdata_q <= {23'h0, tx_kl[rx_waddr]};
        `ADDR_QCPU      : ocl_sh_rdata_q <= q_cpu_reg;
        `ADDR_DBG_QC    : ocl_sh_rdata_q <= dbg_qc;
        `ADDR_LOADERR   : ocl_sh_rdata_q <= {30'h0, load_err};
        `ADDR_LATENCY   : ocl_sh_rdata_q <= reg_latency;
        `ADDR_STATUS    : ocl_sh_rdata_q <= {29'h0, tx_done, replay_busy, lat_run};
        `ADDR_SESSION   : ocl_sh_rdata_q <= reg_session;
        `ADDR_UDP_PORT  : ocl_sh_rdata_q <= reg_udp;
        default         : ocl_sh_rdata_q <= `INVALID_ADDR_RESP;
      endcase
    end
  end
  // NOTE: TX_RADDR reuses rx_waddr as the shared beat index for host readback;
  // the host sets ADDR_RX_WADDR then reads ADDR_TX_DATA_{LO,HI}.

  //---------------------------- the GEL data plane --------------------------
  mmx_dataplane DP (
    .clk (clk_main_a0), .rst (dp_rst), .book_clr (book_clr_r),
    .mac_tdata (mac_tdata), .mac_tkeep (mac_tkeep), .mac_tvalid(mac_tvalid),
    .mac_tlast (mac_tlast), .mac_tready(mac_tready),
    .m_tdata (m_tdata), .m_tkeep (m_tkeep), .m_tvalid (m_tvalid),
    .m_tlast (m_tlast), .m_tready (m_tready),
    .s_wr_en (s_wr_en), .s_wr_addr (pb_addr), .s_wr_data (s_wr_data), .s_commit (s_commit),
    .now_ns (64'h0), .session_id (reg_session), .cfg_udp_port (reg_udp[15:0]),
    .q_cpu_reg (q_cpu_reg), .dbg_qc (dbg_qc), .load_err (load_err)
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
