// =============================================================================
// backtester_stream.c — CONTINUOUS synthetic-feed backtest on real silicon.
// Writes a synthetic mean-reverting series into tick_ram ONCE, then repeatedly
// runs the on-chip hw_backtester on growing prefixes k = 8,16,...,MAX, printing
// one "STEP session k pnl fills" line per point. This traces how the strategy's
// PnL evolves as more market data is processed. Loops forever with fresh feeds
// (new seed each session) until killed (Ctrl-C on the client closes the SSH
// channel -> SIGPIPE here). A `timeout` wrapper on the box bounds the session.
//   usage: backtester_stream <slot> <half_spread> <inv_skew_q31> <seed>
// =============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
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
#define MAXN 512

static pci_bar_handle_t g_h = PCI_BAR_HANDLE_INIT;
static void wr(uint32_t a, uint32_t v){ fpga_pci_poke(g_h,a,v); }
static uint32_t rd(uint32_t a){ uint32_t v=0; fpga_pci_peek(g_h,a,&v); return v; }

static int32_t s[MAXN];
static void gen(unsigned seed){
    srand(seed); int mid=1000, lo=1<<30, hi=-(1<<30);
    for(int i=0;i<MAXN;i++){ int d=(1000-mid)/16, nz=(rand()%25)-12; mid+=d+nz; if(mid<1)mid=1;
        s[i]=mid; if(mid<lo)lo=mid; if(mid>hi)hi=mid; }
    printf("FEED range=[%d..%d]\nSERIES:",lo,hi);
    for(int i=0;i<MAXN;i++) printf(" %d",s[i]);
    printf("\n"); fflush(stdout);
}
static void load(void){ for(int i=0;i<MAXN;i++){ wr(ADDR_TICK_WADDR,i); wr(ADDR_TICK_WDATA,(uint32_t)s[i]); } }

int main(int argc,char**argv){
    signal(SIGPIPE, SIG_DFL);
    int slot=(argc>1)?atoi(argv[1]):0;
    int hs=(argc>2)?atoi(argv[2]):8;
    int isk=(argc>3)?atoi(argv[3]):0;
    unsigned seed=(argc>4)?(unsigned)atoi(argv[4]):1;
    if (fpga_mgmt_init()){ fprintf(stderr,"mgmt init failed\n"); return 1; }
    if (fpga_pci_attach(slot,CL_AXIL_DEMO_APP_PF,CL_AXIL_DEMO_BAR_ID,CL_AXIL_DEMO_PCI_FLAGS,&g_h)){
        fprintf(stderr,"attach failed (backtester AFI loaded on slot %d?)\n",slot); return 1; }
    printf("STREAM start: half_spread=%d inv_skew=%d max_inv=100 (MAXN=%d)\n",hs,isk,MAXN); fflush(stdout);
    wr(ADDR_INV_SKEW,(uint32_t)isk); wr(ADDR_HALF_SPREAD,(uint32_t)hs); wr(ADDR_MAX_INV,100); wr(ADDR_BASE_QTY,1);

    for(unsigned session=0;;session++){
        gen(seed+session); load();
        for(int k=8;k<=MAXN;k+=8){
            wr(ADDR_RESET,1); usleep(60);            // clear accumulators for this prefix
            wr(ADDR_INV_SKEW,(uint32_t)isk); wr(ADDR_HALF_SPREAD,(uint32_t)hs); wr(ADDR_MAX_INV,100); wr(ADDR_BASE_QTY,1);
            wr(ADDR_NTICKS,(uint32_t)k); wr(ADDR_START,1);
            uint32_t done=0; for(int i=0;i<20000 && !done;i++){ done=rd(ADDR_DONE)&1; if(!done)usleep(2); }
            uint32_t lo=rd(ADDR_PNL_LO),hi=rd(ADDR_PNL_HI),fills=rd(ADDR_FILLS);
            int64_t pnl=(int64_t)(((uint64_t)hi<<32)|lo);
            printf("STEP %u %d %lld %u\n", session, k, (long long)pnl, fills);
            if(fflush(stdout)!=0) return 0;          // client closed -> stop
            usleep(55000);                           // ~55 ms/point -> watchable stream
        }
    }
}
