// =============================================================================
// backtester_test.c  —  HARDWARE BACKTESTER on-silicon exerciser (AWS F2)
//
// Drives the hw_backtester CL over the OCL AXI-Lite BAR (same attach + poke/peek
// pattern as fpga/cl/mmx_fpga_test.c and fpga/cl_dataplane/mmx_dataplane_test.cpp):
//   1. attach to the loaded AFI over the OCL BAR,
//   2. reset the backtester, program the ParamBlock scalar fields,
//   3. write the golden tick ROM {100,106,100,94,100} into the on-chip tick_ram,
//   4. set n_ticks = 5 and pulse START,
//   5. poll DONE, then read the 64-bit total_pnl (LO/HI) and the fill counter,
//   6. PASS iff total_pnl == 8 and fills == 4 (the golden values from
//      tb_hw_backtester.vhd: half_spread=5, all skews 0, max_inv=100, base_qty=1).
//
// Register map — MUST match fpga/cl_dataplane/cl_backtester_defines.vh.
// =============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>       // usleep
#include "fpga_pci.h"
#include "fpga_mgmt.h"
#include "cl_axil_reg_access_def.h"   // CL_AXIL_DEMO_APP_PF / _BAR_ID / _PCI_FLAGS

// ---- register map (matches cl_backtester_defines.vh) ----
#define ADDR_INV_SKEW    0x00
#define ADDR_HALF_SPREAD 0x04
#define ADDR_MAX_INV     0x08
#define ADDR_BASE_QTY    0x0C
#define ADDR_NTICKS      0x10
#define ADDR_TICK_WADDR  0x14
#define ADDR_TICK_WDATA  0x18
#define ADDR_START       0x1C
#define ADDR_RESET       0x20
#define ADDR_DONE        0x24
#define ADDR_PNL_LO      0x28
#define ADDR_PNL_HI      0x2C
#define ADDR_FILLS       0x30
#define ADDR_STATUS      0x34
#define ADDR_NTICKS_RB   0x38
#define ADDR_TICK_RDATA  0x3C

// ---- golden test series + expected results (tb_hw_backtester.vhd) ----
static const int32_t ROM[] = { 100, 106, 100, 94, 100 };
#define NTICKS   (int)(sizeof(ROM)/sizeof(ROM[0]))
#define EXP_PNL   8
#define EXP_FILLS 4

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

int main(int argc, char** argv) {
    int slot = (argc > 1) ? atoi(argv[1]) : 0;

    // ---- a. attach to the CL over the OCL AXI-Lite BAR ----
    if (fpga_mgmt_init()) { fprintf(stderr, "fpga_mgmt_init failed\n"); return 1; }
    int rc = fpga_pci_attach(slot, CL_AXIL_DEMO_APP_PF, CL_AXIL_DEMO_BAR_ID,
                             CL_AXIL_DEMO_PCI_FLAGS, &g_h);
    if (rc) {
        fprintf(stderr, "fpga_pci_attach failed rc=%d (is the backtester AFI loaded on slot %d?)\n",
                rc, slot);
        return 1;
    }
    printf("== MMX HARDWARE BACKTESTER on-silicon test (slot %d) ==\n", slot);

    // ---- b. reset the backtester, then program the ParamBlock scalars ----
    wr(ADDR_RESET, 1);        // holds bt_rst ~63 cycles; clears accumulators/state
    usleep(1000);             // let the reset window (and its PCIe write) settle

    wr(ADDR_INV_SKEW,    0);       // inv_skew_q31 = 0 (no inventory skew)
    wr(ADDR_HALF_SPREAD, 5);       // half_spread  = 5 ticks
    wr(ADDR_MAX_INV,     100);     // max_inv      = 100 lots
    wr(ADDR_BASE_QTY,    1);       // base_qty     = 1
    printf("params: inv_skew=0 half_spread=5 max_inv=100 base_qty=1 (kill=0)\n");

    // ---- c. write the golden tick ROM into the on-chip tick_ram ----
    for (int i = 0; i < NTICKS; ++i) {
        wr(ADDR_TICK_WADDR, (uint32_t)i);          // select slot
        wr(ADDR_TICK_WDATA, (uint32_t)ROM[i]);     // tick_ram[i] <= mid
    }
    printf("loaded %d ticks:", NTICKS);
    for (int i = 0; i < NTICKS; ++i) printf(" %d", ROM[i]);
    printf("\n");

    // readback sanity (host readback port continuously tracks TICK_WADDR)
    int rb_ok = 1;
    for (int i = 0; i < NTICKS; ++i) {
        wr(ADDR_TICK_WADDR, (uint32_t)i);
        int32_t got = (int32_t)rd(ADDR_TICK_RDATA);
        if (got != ROM[i]) { rb_ok = 0;
            printf("  tick_ram[%d] readback got=%d exp=%d\n", i, got, ROM[i]); }
    }
    printf("tick_ram readback: %s\n", rb_ok ? "OK" : "MISMATCH");

    // ---- d. set series length and pulse START ----
    wr(ADDR_NTICKS, (uint32_t)NTICKS);
    wr(ADDR_START,  1);

    // ---- e. poll DONE (bounded) ----
    uint32_t done = 0;
    for (int i = 0; i < 100000 && !done; ++i) {
        done = rd(ADDR_DONE) & 0x1;
        if (!done) usleep(10);
    }

    // ---- f. read the 64-bit signed PnL + fill count ----
    uint32_t lo    = rd(ADDR_PNL_LO);
    uint32_t hi    = rd(ADDR_PNL_HI);
    int64_t  pnl   = (int64_t)(((uint64_t)hi << 32) | (uint64_t)lo);
    uint32_t fills = rd(ADDR_FILLS);

    int pass = done && (pnl == EXP_PNL) && (fills == (uint32_t)EXP_FILLS);

    printf("\n==================================================================\n");
    printf("HARDWARE BACKTESTER ON SILICON: done=%u  total_pnl=%lld  fills=%u\n",
           done, (long long)pnl, fills);
    printf("expected: total_pnl=%d fills=%d\n", EXP_PNL, EXP_FILLS);
    printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
    printf("==================================================================\n");

    fpga_pci_detach(g_h);
    return pass ? 0 : 1;
}
