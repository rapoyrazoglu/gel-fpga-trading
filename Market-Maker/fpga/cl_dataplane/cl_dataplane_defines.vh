// cl_dataplane_defines.vh — register map for the FULL GEL data plane CL.
// Host feeds a market-data frame into RX (BRAM replay), reads the emitted OUCH
// order out of TX, and loads the signed ParamBlock. An on-chip counter times
// data-in -> order-out (the wire-to-wire FPGA latency).
`ifndef CL_AXIL_REG_ACCESS_DEFINES
`define CL_AXIL_REG_ACCESS_DEFINES

`define CL_NAME cl_axil_reg_access

// ---- ParamBlock auto-load path (16 words -> param_block_loader, HMAC-checked)
`define ADDR_PB_ADDR    32'h00   // [3:0] ParamBlock word index (0..15)
`define ADDR_PB_DATA    32'h04   // write => push {PB_ADDR,this} into the loader
`define ADDR_PB_COMMIT  32'h08   // write => s_commit pulse (verify + swap in)

// ---- RX market-data frame (host loads beats, then triggers line-rate replay)
`define ADDR_RX_WADDR   32'h0C   // [4:0] beat index to write (0..31)
`define ADDR_RX_DATA_LO 32'h10   // low 32 bits of the 64-bit beat
`define ADDR_RX_DATA_HI 32'h14   // write => store {HI,LO} @ RX[WADDR]
`define ADDR_RX_KEEP    32'h18   // [7:0]=tkeep, [8]=tlast for this beat
`define ADDR_RX_NBEATS  32'h1C   // number of beats in the frame
`define ADDR_RX_GO      32'h20   // write => start replay + latency timer

// ---- TX order frame (host reads the emitted OUCH order back)
`define ADDR_TX_COUNT   32'h24   // read => number of TX beats captured
`define ADDR_TX_RADDR   32'h28   // [4:0] beat index to read (0..31)
`define ADDR_TX_DATA_LO 32'h2C   // read => low 32 of TX[RADDR]
`define ADDR_TX_DATA_HI 32'h30   // read => high 32 of TX[RADDR]
`define ADDR_TX_KEEP    32'h34   // read => {tlast, tkeep}

// ---- config / status
`define ADDR_SESSION    32'h38   // OUCH session id (order_encoder)
`define ADDR_UDP_PORT   32'h3C   // UDP dst port filter (mac_udp_rx)
`define ADDR_QCPU       32'h40   // read => inventory CPU mirror (q_cpu_reg)
`define ADDR_LOADERR    32'h44   // read => param load_err (00 ok/01 hmac/10 stale)
`define ADDR_LATENCY    32'h48   // read => data-in -> order-out latency (cycles)
`define ADDR_STATUS     32'h4C   // read => {..., tx_done, replay_busy}
`define ADDR_RESET      32'h50   // write => reset the data plane + counters
`define ADDR_BOOK_CLEAR 32'h54   // write => clear ONLY the order book (inventory/params persist)
`define ADDR_DBG_QC     32'h58   // read  => pricing stage-1 clamped inventory (debug)

`define INVALID_ADDR_RESP 32'hDEADBEEF
`define AXI_PROT_DEFAULT  3'h0
`define AXI_RESP_OKAY     2'b00

`endif
