// =============================================================================
// backtester_syn.c — SYNTHETIC-FEED hardware backtester (AWS F2)
// Generates a mean-reverting synthetic price series (like intraday market data),
// loads it into the on-chip tick_ram, runs the REAL hw_backtester (same MMX A-S
// pricing + resting-maker fill + mark-to-market) and prints the on-silicon PnL.
//   usage: backtester_syn <slot> <n_ticks> <seed> [half_spread]
// Register map matches cl_backtester_defines.vh (same as backtester_test.c).
// =============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include "fpga_pci.h"
#include "fpga_mgmt.h"
#include "cl_axil_reg_access_def.h"

#define ADDR_INV_SKEW 0x00
#define ADDR_HALF_SPREAD 0x04
#define ADDR_MAX_INV 0x08
#define ADDR_BASE_QTY 0x0C
#define ADDR_NTICKS 0x10
#define ADDR_TICK_WADDR 0x14
#define ADDR_TICK_WDATA 0x18
#define ADDR_START 0x1C
#define ADDR_RESET 0x20
#define ADDR_DONE 0x24
#define ADDR_PNL_LO 0x28
#define ADDR_PNL_HI 0x2C
#define ADDR_FILLS 0x30
#define ADDR_TICK_RDATA 0x3C

static pci_bar_handle_t g_h = PCI_BAR_HANDLE_INIT;
static void wr(uint32_t a, uint32_t v){ fpga_pci_poke(g_h,a,v); }
static uint32_t rd(uint32_t a){ uint32_t v=0; fpga_pci_peek(g_h,a,&v); return v; }

int main(int argc, char** argv){
    int slot   = (argc>1)? atoi(argv[1]) : 0;
    int n      = (argc>2)? atoi(argv[2]) : 64;
    unsigned seed = (argc>3)? (unsigned)atoi(argv[3]) : 1;
    int hs     = (argc>4)? atoi(argv[4]) : 8;
    if(n<1) n=1; if(n>1000) n=1000;

    if (fpga_mgmt_init()) { fprintf(stderr,"fpga_mgmt_init failed\n"); return 1; }
    if (fpga_pci_attach(slot, CL_AXIL_DEMO_APP_PF, CL_AXIL_DEMO_BAR_ID,
                        CL_AXIL_DEMO_PCI_FLAGS, &g_h)) {
        fprintf(stderr,"attach failed (is the backtester AFI loaded on slot %d?)\n",slot); return 1;
    }

    // ---- generate a mean-reverting synthetic mid series (integer prices) ----
    static int32_t s[1000];
    srand(seed);
    int mid=1000, lo=1000000, hiv=-1000000;
    for(int i=0;i<n;i++){
        int drift=(1000-mid)/16;            // pull toward the mean (mean reversion)
        int noise=(rand()%25)-12;           // -12..+12 tick noise
        mid+=drift+noise; if(mid<1)mid=1;
        s[i]=mid; if(mid<lo)lo=mid; if(mid>hiv)hiv=mid;
    }

    printf("== MMX SYNTHETIC-FEED BACKTEST on silicon (slot %d) ==\n", slot);
    printf("feed: %d synthetic mean-reverting ticks (seed=%u), price range [%d..%d]\n", n, seed, lo, hiv);
    printf("params: half_spread=%d inv_skew=0 max_inv=100 base_qty=1\n", hs);

    // ---- reset + params ----
    wr(ADDR_RESET,1); usleep(1000);
    wr(ADDR_INV_SKEW,0); wr(ADDR_HALF_SPREAD,(uint32_t)hs); wr(ADDR_MAX_INV,100); wr(ADDR_BASE_QTY,1);

    // ---- load the series into tick_ram ----
    for(int i=0;i<n;i++){ wr(ADDR_TICK_WADDR,(uint32_t)i); wr(ADDR_TICK_WDATA,(uint32_t)s[i]); }

    // print series so the CLI can draw a sparkline
    printf("SERIES:");
    for(int i=0;i<n;i++) printf(" %d", s[i]);
    printf("\n");

    // ---- run ----
    wr(ADDR_NTICKS,(uint32_t)n); wr(ADDR_START,1);
    uint32_t done=0;
    for(int i=0;i<200000 && !done;i++){ done=rd(ADDR_DONE)&1; if(!done)usleep(5); }

    uint32_t plo=rd(ADDR_PNL_LO), phi=rd(ADDR_PNL_HI), fills=rd(ADDR_FILLS);
    int64_t pnl=(int64_t)(((uint64_t)phi<<32)|plo);

    // on-chip processing: ~6 cycles/tick (ADDR/WAIT/FILL/MULT/RESV/QUOTE) @ 4 ns
    double ns_per_tick = 6*4.0;
    printf("\n==================================================================\n");
    printf("SYNTHETIC-FEED RESULT (on real silicon, AWS F2 / VU47P):\n");
    printf("  ticks processed = %d   fills = %u\n", n, fills);
    printf("  total_pnl = %lld   (realized + marked-to-market at last price)\n", (long long)pnl);
    printf("  on-chip rate = ~%.0f ns/tick (6 cyc @ 250 MHz) = ~%.1f M ticks/s, done=%u\n",
           ns_per_tick, 1000.0/ns_per_tick, done);
    printf("RESULT: %s\n", done? "OK (ran on silicon)":"TIMEOUT");
    printf("==================================================================\n");
    fpga_pci_detach(g_h);
    return done?0:1;
}
