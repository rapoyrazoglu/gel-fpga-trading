// =============================================================================
// multiasset_test.c — on-silicon parity check for MULTI-ASSET parallel pricing.
//
// Drives cl_axil_reg_access (cl_multiasset.sv) over the OCL AXI-Lite BAR: for a
// few test vectors it writes EACH of the N lanes' (s, q, ParamBlock) input
// registers, pulses ADDR_STROBE (valid_in to all lanes in the SAME cycle), then
// reads back EACH lane's (bid, ask, qty, cancel) and compares to the host C++
// reference compute_quote() run per lane. PASS iff every lane is bit-exact.
//
// Attach + wr()/rd() poke/peek pattern is identical to fpga/cl/mmx_fpga_test.c
// and fpga/cl_dataplane/mmx_dataplane_test.cpp. Plain C (compute_quote is C).
//
// Register map MUST match fpga/cl_dataplane/cl_multiasset_defines.vh.
// =============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>            // usleep
#include "fpga_pci.h"
#include "fpga_mgmt.h"
#include "cl_axil_reg_access_def.h"   // CL_AXIL_DEMO_APP_PF / _BAR_ID / _PCI_FLAGS

// ---- register map (must match cl_multiasset_defines.vh) ---------------------
#define N_LANES        4
#define LANE_STRIDE    0x20
#define LANE_S         0x00
#define LANE_Q         0x04
#define LANE_INV_SKEW  0x08
#define LANE_HALF_SPRD 0x0C
#define LANE_MAX_INV   0x10
#define LANE_BASE_QTY  0x14
#define LANE_SKEWS     0x18   /* [15:0]=bid_skew, [31:16]=ask_skew */
#define LANE_KILL      0x1C

#define ADDR_STROBE    0x80
#define ADDR_STATUS    0x84   /* [1]=quote_valid, [0]=result_ready */
#define ADDR_RESET     0x88
#define ADDR_N_LANES   0x8C

#define OUT_BASE       0x100
#define OUT_STRIDE     0x10
#define OUT_BID        0x00
#define OUT_ASK        0x04
#define OUT_QTY        0x08
#define OUT_CANCEL     0x0C

#define STATUS_READY   0x1u
#define STATUS_QV_LIVE 0x2u

// ---- poke/peek helpers — identical pattern to mmx_fpga_test.c ---------------
static pci_bar_handle_t g_h = PCI_BAR_HANDLE_INIT;

static void wr(uint32_t a, uint32_t v) {
    int rc = fpga_pci_poke(g_h, a, v);
    if (rc) fprintf(stderr, "  [warn] poke @0x%03x = 0x%08x failed rc=%d\n", a, v, rc);
}
static uint32_t rd(uint32_t a) {
    uint32_t v = 0;
    int rc = fpga_pci_peek(g_h, a, &v);
    if (rc) fprintf(stderr, "  [warn] peek @0x%03x failed rc=%d\n", a, rc);
    return v;
}

static uint32_t lane_in (int lane, uint32_t field) { return (uint32_t)lane*LANE_STRIDE + field; }
static uint32_t lane_out(int lane, uint32_t field) { return OUT_BASE + (uint32_t)lane*OUT_STRIDE + field; }

// ---- host reference — identical math to host/pricing/pricing_engine.h -------
static int32_t sat32(int64_t v){
    if (v >  2147483647LL) return  2147483647;
    if (v < -2147483648LL) return -2147483648;
    return (int32_t)v;
}
static void compute_quote(int32_t s,int32_t q,int32_t inv,int32_t hs,uint32_t maxinv,
                          uint32_t bq,int16_t bsk,int16_t ask_sk,uint8_t kill,
                          int32_t*bid,int32_t*ask,uint32_t*qty,uint8_t*cancel){
    int64_t cap=(int64_t)maxinv;
    uint8_t k = kill | (((int64_t)q>cap)||((int64_t)q<-cap));
    int64_t qc=q; if(qc>cap)qc=cap; if(qc<-cap)qc=-cap;
    int64_t prod=qc*(int64_t)inv;
    int64_t skew=(prod+(1LL<<30))>>31;
    int64_t r=(int64_t)s-skew;
    *bid=sat32(r-(int64_t)hs-(int64_t)bsk);
    *ask=sat32(r+(int64_t)hs+(int64_t)ask_sk);
    *qty=bq; *cancel=k;
}

// ---- one lane's inputs -------------------------------------------------------
typedef struct {
    int32_t  s, q, inv_skew, half_spread;
    uint32_t max_inv, base_qty;
    int16_t  bid_skew, ask_skew;
    uint8_t  kill;
} lane_in_t;

// A few vectors, each with N_LANES distinct lane inputs (mirrors the RTL tb).
static const lane_in_t VEC[][N_LANES] = {
  // vector 0 : mid / high+neg / over-cap kill / forced-kill big skew
  { { 100000,   10, 1000000,  50,  100,  5,  3,  4, 0 },
    { 250000,  -25,  500000,  75,  200,  7, -2,  1, 0 },
    {  50000,  999, 2000000,  20,   50,  3,  0,  0, 0 },
    { -30000, -100,-3000000,  10,  500,  9,  5, -5, 1 } },
  // vector 1 : neg mid / big price / zeros / int32 max
  { {-100000,   -5, 7000000,  11,   80,  4,  1,  2, 0 },
    { 999999,  199,  123456, 100,  150,  6,  9, -9, 0 },
    {      0,    0,       0,   0,   10,  1,  0,  0, 0 },
    { 2147483647, 3, 2000000000, 1000, 1000, 2, 100, 100, 0 } },
  // vector 2 : neg inventory beyond -max_inv / mixed
  { {  12345, -500, 1500000,  30,  100,  5, -1, -1, 0 },
    {  54321,   40, -900000,  60,  300,  8,  2,  3, 0 },
    { -77777,  -10,  250000,  15,  250,  3,  0,  7, 0 },
    {  88888,   88,  333333,  45,  200,  6, -4,  4, 0 } },
};
#define N_VEC ((int)(sizeof(VEC)/sizeof(VEC[0])))

int main(int argc, char** argv) {
    int slot = (argc > 1) ? atoi(argv[1]) : 0;

    if (fpga_mgmt_init()) { fprintf(stderr, "fpga_mgmt_init failed\n"); return 1; }
    int rc = fpga_pci_attach(slot, CL_AXIL_DEMO_APP_PF, CL_AXIL_DEMO_BAR_ID,
                             CL_AXIL_DEMO_PCI_FLAGS, &g_h);
    if (rc) { fprintf(stderr, "fpga_pci_attach failed rc=%d (is the multi-asset AFI loaded on slot %d?)\n",
                      rc, slot); return 1; }

    uint32_t hw_lanes = rd(ADDR_N_LANES);
    printf("== MMX MULTI-ASSET parallel pricing on silicon (slot %d) ==\n", slot);
    printf("HW lane count = %u (host expects %d)%s\n", hw_lanes, N_LANES,
           (hw_lanes == N_LANES) ? "" : "  <WARN: mismatch>");

    wr(ADDR_RESET, 1);
    usleep(1000);                    // let the ~31-cycle reset window settle

    int total_mismatch = 0;
    for (int v = 0; v < N_VEC; ++v) {
        // ---- write every lane's inputs ----
        for (int L = 0; L < N_LANES; ++L) {
            const lane_in_t* x = &VEC[v][L];
            uint32_t skews = ((uint32_t)(uint16_t)x->ask_skew << 16) | (uint16_t)x->bid_skew;
            wr(lane_in(L, LANE_S),          (uint32_t)x->s);
            wr(lane_in(L, LANE_Q),          (uint32_t)x->q);
            wr(lane_in(L, LANE_INV_SKEW),   (uint32_t)x->inv_skew);
            wr(lane_in(L, LANE_HALF_SPRD),  (uint32_t)x->half_spread);
            wr(lane_in(L, LANE_MAX_INV),    x->max_inv);
            wr(lane_in(L, LANE_BASE_QTY),   x->base_qty);
            wr(lane_in(L, LANE_SKEWS),      skews);
            wr(lane_in(L, LANE_KILL),       (uint32_t)(x->kill & 1));
        }

        // ---- pulse valid_in to ALL lanes in the same cycle ----
        wr(ADDR_STROBE, 1);

        // ---- wait for the 5-stage pipeline result (PCIe round-trip >> 5 cyc) --
        uint32_t status = 0;
        for (int i = 0; i < 1000; ++i) {
            status = rd(ADDR_STATUS);
            if (status & STATUS_READY) break;
            usleep(10);
        }
        if (!(status & STATUS_READY))
            printf("vec %d: WARN result_ready never set (status=0x%08x)\n", v, status);

        // ---- read back + compare each lane ----
        for (int L = 0; L < N_LANES; ++L) {
            const lane_in_t* x = &VEC[v][L];
            int32_t  fbid = (int32_t)rd(lane_out(L, OUT_BID));
            int32_t  fask = (int32_t)rd(lane_out(L, OUT_ASK));
            uint32_t fqty =          rd(lane_out(L, OUT_QTY));
            uint32_t fcan =          rd(lane_out(L, OUT_CANCEL)) & 1;

            int32_t  rbid, rask; uint32_t rqty; uint8_t rcan;
            compute_quote(x->s, x->q, x->inv_skew, x->half_spread, x->max_inv,
                          x->base_qty, x->bid_skew, x->ask_skew, x->kill,
                          &rbid, &rask, &rqty, &rcan);

            int bad = (fbid != rbid) || (fask != rask) || (fqty != rqty) || (fcan != (rcan & 1));
            if (bad) total_mismatch++;
            printf("vec %d lane %d: FPGA bid=%d ask=%d qty=%u cancel=%u | host bid=%d ask=%d qty=%u cancel=%u  %s\n",
                   v, L, fbid, fask, fqty, fcan, rbid, rask, rqty, rcan & 1,
                   bad ? "MISMATCH" : "ok");
        }
    }

    int checks = N_VEC * N_LANES;
    printf("\n=== MULTI-ASSET ON-SILICON PARITY: %d lane-checks, %d mismatches -> %s ===\n",
           checks, total_mismatch,
           total_mismatch == 0 ? "ALL LANES BIT-EXACT ON HARDWARE" : "MISMATCH");

    fpga_pci_detach(g_h);
    return total_mismatch == 0 ? 0 : 1;
}
