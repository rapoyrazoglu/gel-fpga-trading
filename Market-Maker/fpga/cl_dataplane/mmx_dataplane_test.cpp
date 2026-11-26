// =============================================================================
// mmx_dataplane_test.cpp  —  FULL GEL DATA PLANE on-silicon exerciser (AWS F2)
//
// NOTE ON FILENAME/LANGUAGE: the task asked for `mmx_dataplane_test.c`, but the
// host IPC signer we are required to REUSE (host/ipc/hmac.h + sha256.h +
// param_block.h) is C++-only (namespace mmx, std::vector, member functions,
// static_assert/offsetof on a C++ struct). Per the task's own escape hatch
// ("if they are C++-only, write it as .cpp and note that") this file is C++ and
// MUST be compiled with g++ and linked against host/ipc/hmac.cpp + sha256.cpp.
//
// What it does (single OCL AXI-Lite BAR, poke/peek — same attach pattern as
// fpga/cl/mmx_fpga_test.c):
//   1. attach to the loaded AFI over the OCL BAR,
//   2. configure UDP port + OUCH session, reset the data plane,
//   3. build a *host-signed* 64-byte ParamBlock (HMAC-SHA256, key = 0xAB*32),
//      serialize it to 16 little-endian 32-bit words, stream them into the
//      on-chip param_block_loader and COMMIT (on-chip HMAC verify + swap-in),
//   4. build ONE market-data frame (62-byte Ethernet/IPv4/UDP wrapping a 20-byte
//      ITCH-lite tick), load it into the RX replay BRAM as 8x 64-bit beats and
//      fire RX_GO -> the frame streams into the pipeline at one beat/cycle,
//   5. poll STATUS until the OUCH order frame is emitted on TX,
//   6. read the TX order back beat-by-beat, decode beat 2 = bid:ask,
//   7. read the wire-to-wire latency counter (data-in -> order-out) and the
//      inventory mirror, and print a PASS/summary line.
//
// This proves the WHOLE pipeline fires end-to-end on real hardware:
//   mac_udp_rx -> feed_parser -> order_book -> pricing_engine_beh ->
//   quote_decider -> order_encoder, plus the HMAC-verified param_block_loader.
//
// Pin-for-pin format sources (do not guess — these were read exactly):
//   * fpga/cl_dataplane/cl_dataplane_defines.vh   (ADDR_* register offsets)
//   * fpga/cl_dataplane/cl_dataplane.sv           (write/read/STATUS bit layout)
//   * fpga/rtl/tb_mmx_dataplane.vhd               (frame + beat packing)
//   * fpga/rtl/tb_param_load.vhd                  (16-word ParamBlock order)
//   * fpga/rtl/feed_parser.vhd / mac_udp_rx.vhd   (payload offsets + header check)
//   * fpga/rtl/order_encoder.vhd                  (TX beat layout, beat2=bid:ask)
//   * host/ipc/param_block.h + hmac.h             (byte-exact block + signer)
//   * fpga/cl/mmx_fpga_test.c / _v3.c             (attach + wr()/rd() helpers)
// =============================================================================

#include <cstdio>
#include <cstdlib>           // atoi
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <vector>
#include <unistd.h>          // usleep

// ---- AWS FPGA C SDK (C headers; guard against C++ name mangling) ------------
extern "C" {
#include "fpga_pci.h"
#include "fpga_mgmt.h"
#include "cl_axil_reg_access_def.h"   // CL_AXIL_DEMO_APP_PF / _BAR_ID / _PCI_FLAGS
}

// ---- host IPC signer we reuse verbatim (C++) --------------------------------
//   -I <repo>/host/ipc  so these resolve, exactly like host/ipc/hmac.cpp does.
#include "param_block.h"     // mmx::ParamBlock (64B, offsets pinned by static_assert)
#include "hmac.h"            // mmx::sign_param_block(block, key, key_len)

// -----------------------------------------------------------------------------
// Register map — MUST match fpga/cl_dataplane/cl_dataplane_defines.vh
// -----------------------------------------------------------------------------
#define ADDR_PB_ADDR    0x00   // [3:0] ParamBlock word index (0..15)
#define ADDR_PB_DATA    0x04   // write => push {PB_ADDR,this} into the loader
#define ADDR_PB_COMMIT  0x08   // write => s_commit pulse (verify + swap in)
#define ADDR_RX_WADDR   0x0C   // [4:0] beat index to write (0..31); ALSO the TX readback index
#define ADDR_RX_DATA_LO 0x10   // low 32 bits of the 64-bit beat (latched)
#define ADDR_RX_DATA_HI 0x14   // write => store {HI,LO} @ RX[WADDR]
#define ADDR_RX_KEEP    0x18   // [7:0]=tkeep, [8]=tlast for this beat
#define ADDR_RX_NBEATS  0x1C   // number of beats in the frame
#define ADDR_RX_GO      0x20   // write => start replay + latency timer
#define ADDR_TX_COUNT   0x24   // read => number of TX beats captured (tx_wr_ptr)
#define ADDR_TX_RADDR   0x28   // read => current shared beat index (mirror of rx_waddr)
#define ADDR_TX_DATA_LO 0x2C   // read => low 32 of TX[RX_WADDR]  (= ask leg on beat 2)
#define ADDR_TX_DATA_HI 0x30   // read => high 32 of TX[RX_WADDR] (= bid leg on beat 2)
#define ADDR_TX_KEEP    0x34   // read => {tlast(bit8), tkeep[7:0]}
#define ADDR_SESSION    0x38   // OUCH session id (order_encoder header)
#define ADDR_UDP_PORT   0x3C   // UDP dst port filter (mac_udp_rx)
#define ADDR_QCPU       0x40   // read => inventory CPU mirror (q_cpu_reg, signed)
#define ADDR_LOADERR    0x44   // read => param load_err (00 ok / 01 hmac / 10 stale)
#define ADDR_LATENCY    0x48   // read => data-in -> order-out latency (cycles)
#define ADDR_STATUS     0x4C   // read => {..., tx_done, replay_busy, lat_run}
#define ADDR_RESET      0x50   // write => reset the data plane + counters

// STATUS bit layout — from cl_dataplane.sv (the actual silicon):
//   ocl_sh_rdata_q <= {29'h0, tx_done, replay_busy, lat_run};
// so bit0=lat_run, bit1=replay_busy, bit2=tx_done. (The defines-header comment
// abbreviates this as "{..., tx_done, replay_busy}"; the task brief called
// tx_done "bit1" but the synthesized RTL places it at bit2 — we follow the RTL.)
#define STATUS_LAT_RUN      0x1u
#define STATUS_REPLAY_BUSY  0x2u
#define STATUS_TX_DONE      0x4u

// ITCH-lite message-type codes — from fpga/rtl/mmx_pkg.vhd
#define MSG_ADD   0x41   // 'A'
#define MSG_DEL   0x44   // 'D'
#define MSG_EXEC  0x45   // 'E'
#define MSG_TRADE 0x50   // 'P'

// Feed-side codes (feed_parser: payload byte 11)
#define SIDE_BID  0x00
#define SIDE_ASK  0x01

static const uint16_t UDP_PORT = 0x1F90;      // UDP 8080 (matches cfg_udp_port)
static const uint32_t SESSION  = 0xDEADBEEFu;  // OUCH session id

// -----------------------------------------------------------------------------
// poke/peek helpers — identical pattern to fpga/cl/mmx_fpga_test_v3.c
// -----------------------------------------------------------------------------
static pci_bar_handle_t g_h = PCI_BAR_HANDLE_INIT;

static void wr(uint32_t a, uint32_t v) {
    int rc = fpga_pci_poke(g_h, a, v);
    if (rc) fprintf(stderr, "  [warn] poke @0x%02x = 0x%08x failed rc=%d\n", a, v, rc);
}
static uint32_t rd(uint32_t a) {
    uint32_t v = 0;
    int rc = fpga_pci_peek(g_h, a, &v);
    if (rc) fprintf(stderr, "  [warn] peek @0x%02x failed rc=%d\n", a, rc);
    return v;
}

// -----------------------------------------------------------------------------
// Build the signed 64-byte ParamBlock and serialize to 16 little-endian words.
//
// Word order (w0..w15) is just the 64-byte struct laid out little-endian; the
// field offsets are pinned by static_assert in host/ipc/param_block.h, so the
// serialization below matches tb_param_load.vhd / tb_mmx_dataplane.vhd exactly:
//   w0  inv_skew_q31            w1  half_spread
//   w2  max_inv                 w3  base_qty
//   w4  {bid_skew[31:16], kill_switch[15:8], regime_id[7:0]}
//   w5  {requote_ticks[31:16], ask_skew[15:0]}
//   w6  epoch_ns[31:0]          w7  epoch_ns[63:32]
//   w8..w11 reserved[16] = 0    w12..w15 HMAC-SHA256 tag (truncated to 16 bytes)
//
// The FPGA re-runs HMAC-SHA256 over bytes [0,48) with key 0xAB*32 and rejects
// any block whose trailing 16-byte tag does not match -> load_err=01. We use the
// SAME "sane" params proven in the testbenches (half_spread=5, max_inv=100,
// base_qty=10, requote_ticks=1, everything else 0) so the on-chip HMAC passes
// and load_err reads 00. epoch_ns MUST be 0: the CL ties now_ns=0, so the design
// runs its internal ns counter and a nonzero epoch would look "stale" (err=10).
// -----------------------------------------------------------------------------
static void build_signed_param_words(uint32_t words[16]) {
    std::vector<std::uint8_t> key(32, 0xab);   // key = 32 bytes of 0xAB (host/main.cpp)

    mmx::ParamBlock pb{};                       // zero-init: reserved[] and hmac[] = 0
    pb.inv_skew_q31   = 0;                       // no inventory skew
    pb.half_spread    = 5;                       // modest half-spread (ticks)
    pb.max_inv        = 100;                     // inventory cap
    pb.base_qty       = 10;                      // default order size
    pb.regime_id      = mmx::kRegimeRanging;     // 0 = normal
    pb.kill_switch    = 0;                        // NOT killed -> engine may place
    pb.bid_skew_ticks = 0;
    pb.ask_skew_ticks = 0;
    pb.requote_ticks  = 1;                        // |dprice| threshold to re-quote
    pb.epoch_ns       = 0;                        // 0 => never stale vs internal now_ns

    // Sign in place: writes HMAC-SHA256(key, bytes[0,48))[0:16] into pb.hmac.
    mmx::sign_param_block(pb, key.data(), key.size());

    // Serialize the 64-byte block to 16 little-endian 32-bit words (endian-safe).
    std::uint8_t raw[sizeof(mmx::ParamBlock)];
    std::memcpy(raw, &pb, sizeof(raw));          // sizeof(ParamBlock) == 64
    for (int i = 0; i < 16; ++i) {
        words[i] =  (uint32_t)raw[4*i + 0]
                 | ((uint32_t)raw[4*i + 1] <<  8)
                 | ((uint32_t)raw[4*i + 2] << 16)
                 | ((uint32_t)raw[4*i + 3] << 24);
    }
}

// -----------------------------------------------------------------------------
// Build a 62-byte Ethernet/IPv4/UDP frame carrying one 20-byte ITCH-lite tick,
// then load it into the RX replay BRAM as 8x 64-bit beats.
//
// Header bytes the RTL checks (mac_udp_rx.vhd):
//   ethertype @12..13 = 0x0800, IP proto @23 = 0x11, UDP dst port @36..37 = port.
// Payload starts at byte 42 (42-byte header). ITCH-lite payload (feed_parser.vhd,
// big-endian fields):
//   [0]   msg_type   [1..8] ts_ns   [9..10] ticker_id   [11] side(00=BID/01=ASK)
//   [12..15] price (signed, BE)     [16..19] size (unsigned, BE)
//
// Beat packing matches tb_mmx_dataplane.vhd: data(i*8+7 downto i*8) := frame[k*8+i],
// i.e. lane 0 = frame[k*8+0] = the least-significant byte of DATA_LO. So:
//   DATA_LO = frame[k*8+0..3] little-endian,  DATA_HI = frame[k*8+4..7] little-endian.
// 62 bytes => beats 0..6 carry 8 valid lanes (tkeep=0xFF), beat 7 carries the
// final 6 lanes (bytes 56..61, tkeep=0x3F) and asserts tlast.
// -----------------------------------------------------------------------------
static void load_itch_frame(uint8_t  msg_type,
                            uint8_t  side,
                            uint16_t ticker,
                            int32_t  price,
                            uint32_t size) {
    uint8_t frame[64];
    std::memset(frame, 0, sizeof(frame));        // 62 bytes used; 62..63 stay 0

    // ---- Ethernet / IPv4 / UDP header fields the parser inspects ----
    frame[12] = 0x08; frame[13] = 0x00;          // ethertype = IPv4
    frame[14] = 0x45;                            // IP ver/ihl (cosmetic)
    frame[23] = 0x11;                            // IP protocol = UDP
    frame[36] = (uint8_t)(UDP_PORT >> 8);        // UDP dst port hi
    frame[37] = (uint8_t)(UDP_PORT & 0xFF);      // UDP dst port lo

    // ---- 20-byte ITCH-lite payload at offset 42 (all fields big-endian) ----
    uint8_t* pl = &frame[42];
    pl[0]  = msg_type;
    // pl[1..8] ts_ns = 0 (already zeroed)
    pl[9]  = (uint8_t)(ticker >> 8);   pl[10] = (uint8_t)(ticker & 0xFF);
    pl[11] = side;
    pl[12] = (uint8_t)((uint32_t)price >> 24); pl[13] = (uint8_t)((uint32_t)price >> 16);
    pl[14] = (uint8_t)((uint32_t)price >>  8); pl[15] = (uint8_t)((uint32_t)price      );
    pl[16] = (uint8_t)(size >> 24);    pl[17] = (uint8_t)(size >> 16);
    pl[18] = (uint8_t)(size >>  8);    pl[19] = (uint8_t)(size      );

    // ---- stream the 8 beats into the RX replay BRAM ----
    for (int k = 0; k < 8; ++k) {
        uint32_t lo =  (uint32_t)frame[k*8 + 0]
                    | ((uint32_t)frame[k*8 + 1] <<  8)
                    | ((uint32_t)frame[k*8 + 2] << 16)
                    | ((uint32_t)frame[k*8 + 3] << 24);
        uint32_t hi =  (uint32_t)frame[k*8 + 4]
                    | ((uint32_t)frame[k*8 + 5] <<  8)
                    | ((uint32_t)frame[k*8 + 6] << 16)
                    | ((uint32_t)frame[k*8 + 7] << 24);

        uint32_t tkeep = (k < 7) ? 0xFF : 0x3F;   // last beat: 6 valid lanes
        uint32_t tlast = (k == 7) ? 1u : 0u;
        uint32_t keep  = (tkeep & 0xFF) | (tlast << 8);   // {tlast, tkeep} -> RX_KEEP[8:0]

        wr(ADDR_RX_WADDR,   (uint32_t)k);   // select beat slot (also gates the store below)
        wr(ADDR_RX_DATA_LO, lo);            // latch low 32
        wr(ADDR_RX_DATA_HI, hi);            // store {HI,LO} @ RX[k]
        wr(ADDR_RX_KEEP,    keep);          // per-beat {tlast,tkeep}
    }
    wr(ADDR_RX_NBEATS, 8);
}

// =============================================================================
int main(int argc, char** argv) {
    int slot = (argc > 1) ? atoi(argv[1]) : 0;

    // ---- a. attach to the CL over the OCL AXI-Lite BAR (mmx_fpga_test.c) ----
    if (fpga_mgmt_init()) {
        fprintf(stderr, "fpga_mgmt_init failed\n");
        return 1;
    }
    int rc = fpga_pci_attach(slot, CL_AXIL_DEMO_APP_PF, CL_AXIL_DEMO_BAR_ID,
                             CL_AXIL_DEMO_PCI_FLAGS, &g_h);
    if (rc) {
        fprintf(stderr, "fpga_pci_attach failed rc=%d (is the data-plane AFI loaded on slot %d?)\n",
                rc, slot);
        return 1;
    }
    printf("== MMX FULL DATA PLANE on-silicon test (slot %d) ==\n", slot);

    // ---- b. config: UDP port, OUCH session, then reset the data plane ----
    wr(ADDR_UDP_PORT, UDP_PORT);
    wr(ADDR_SESSION,  SESSION);
    wr(ADDR_RESET,    1);              // holds dp_rst ~31 cycles; clears book/inv/params
    usleep(1000);                     // let the reset window (and its PCIe write) settle

    // ---- c. build + sign a ParamBlock, stream 16 words, COMMIT, check err ----
    uint32_t pbw[16];
    build_signed_param_words(pbw);

    printf("ParamBlock (16 LE words, HMAC key=0xAB*32):\n");
    for (int i = 0; i < 16; ++i)
        printf("  w%-2d = 0x%08x%s", i, pbw[i], (i % 4 == 3) ? "\n" : "");

    // Self-check vs the known-good tag proven in tb_param_load.vhd /
    // tb_mmx_dataplane.vhd for these exact params (half_spread=5, max_inv=100,
    // base_qty=10, requote=1, epoch=0). If the host signer + layout are correct
    // the trailing four words match byte-for-byte.
    static const uint32_t kExpectTag[4] =
        { 0xb55305c9u, 0x34530487u, 0x88fbee6au, 0x2d969112u };
    bool tag_ok = (pbw[12]==kExpectTag[0] && pbw[13]==kExpectTag[1] &&
                   pbw[14]==kExpectTag[2] && pbw[15]==kExpectTag[3]);
    printf("  host-signer tag self-check vs testbench golden: %s\n",
           tag_ok ? "MATCH" : "DIFFERS (params or signer changed)");

    for (int i = 0; i < 16; ++i) {
        wr(ADDR_PB_ADDR, (uint32_t)i);   // select word index
        wr(ADDR_PB_DATA, pbw[i]);        // push {index,data} into param_block_loader
    }
    wr(ADDR_PB_COMMIT, 1);               // s_commit pulse -> on-chip HMAC verify + swap
    usleep(5000);                        // HMAC-SHA256 (~1.1us @250MHz) + load/swap << 5ms

    uint32_t load_err = rd(ADDR_LOADERR) & 0x3;
    printf("ParamBlock load_err = 0x%x (%s)\n", load_err,
           load_err == 0 ? "OK: HMAC authenticated, params live"
         : load_err == 1 ? "HMAC MISMATCH"
         : load_err == 2 ? "STALE (epoch/now_ns)" : "unknown");

    // ---- d. build ONE ITCH-lite market-data tick and load it into RX ----
    // A single ADD seeds one book side; the pipeline still emits an OUCH order
    // (quote_decider PLACEs on the first quote, whose bid/ask differ from the
    // reset value 0 by more than requote_ticks). Same wire format the tb uses.
    const uint8_t  MTYPE  = MSG_ADD;
    const uint8_t  SIDE   = SIDE_BID;
    const uint16_t TICKER = 1;
    const int32_t  PRICE  = 100;
    const uint32_t SIZE   = 50;
    printf("Loading RX frame: ITCH ADD %s ticker=%u price=%d size=%u (8 beats)\n",
           SIDE == SIDE_BID ? "BID" : "ASK", TICKER, PRICE, SIZE);
    load_itch_frame(MTYPE, SIDE, TICKER, PRICE, SIZE);

    // ---- e. fire RX_GO; poll STATUS until the TX order frame is emitted ----
    wr(ADDR_RX_GO, 1);

    uint32_t status = 0, tx_count = 0;
    bool tx_done = false;
    for (int i = 0; i < 5000 && !tx_done; ++i) {
        status   = rd(ADDR_STATUS);
        tx_count = rd(ADDR_TX_COUNT) & 0x3F;
        // done when the encoder asserts tlast (tx_done, bit2) or a full frame landed
        if ((status & STATUS_TX_DONE) || tx_count >= 8) tx_done = true;
        else usleep(20);
    }
    tx_count = rd(ADDR_TX_COUNT) & 0x3F;
    printf("RX replay fired. STATUS=0x%08x tx_done=%d replay_busy=%d tx_count=%u\n",
           status, (status & STATUS_TX_DONE) ? 1 : 0,
           (status & STATUS_REPLAY_BUSY) ? 1 : 0, tx_count);

    // ---- f. read the emitted OUCH order back, decode beat 2 = bid:ask ----
    int32_t bid = 0, ask = 0;
    int nbeats  = (int)tx_count;
    if (nbeats > 32) nbeats = 32;
    printf("TX order frame (%d beats):\n", nbeats);
    for (int b = 0; b < nbeats; ++b) {
        wr(ADDR_RX_WADDR, (uint32_t)b);      // shared beat index for TX readback
        uint32_t lo = rd(ADDR_TX_DATA_LO);
        uint32_t hi = rd(ADDR_TX_DATA_HI);
        uint32_t kp = rd(ADDR_TX_KEEP);      // {tlast(bit8), tkeep[7:0]}
        printf("  beat %d: hi=0x%08x lo=0x%08x keep=0x%03x%s\n",
               b, hi, lo, kp, (kp & 0x100) ? "  <tlast>" : "");
        if (b == 2) {                        // beat 2: [63:32]=bid, [31:0]=ask
            bid = (int32_t)hi;
            ask = (int32_t)lo;
        }
    }

    // ---- g. wire-to-wire latency + inventory mirror + PASS/summary ----
    uint32_t lat_cyc = rd(ADDR_LATENCY);          // data-in -> order-out cycles
    uint32_t lat_ns  = lat_cyc * 4;               // 250 MHz => 4 ns/cycle
    int32_t  q_cpu   = (int32_t)rd(ADDR_QCPU);    // inventory mirror (signed)

    bool pass = tx_done && (nbeats >= 3) && (bid < ask) && (load_err == 0);

    printf("\n==================================================================\n");
    printf("FULL DATA PLANE ON SILICON: order emitted=%s, bid=%d ask=%d, "
           "latency=%u cycles = %u ns, load_err=%u, q_cpu=%d\n",
           tx_done ? "YES" : "NO", bid, ask, lat_cyc, lat_ns, load_err, q_cpu);
    printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
    printf("==================================================================\n");

    fpga_pci_detach(g_h);
    return pass ? 0 : 1;
}
