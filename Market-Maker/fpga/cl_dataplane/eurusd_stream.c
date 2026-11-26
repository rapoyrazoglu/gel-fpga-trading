// =============================================================================
// eurusd_stream.c — REAL exchange-feed emulation on the FULL GEL data plane.
// Streams a synthetic EUR/USD price feed as genuine ITCH frames THROUGH the real
// 80 ns pipeline (mac_udp_rx -> feed_parser -> order_book -> pricing_engine ->
// quote_decider -> order_encoder), reads the emitted OUCH quote each tick, closes
// the loop by reporting our fills back to the on-chip inventory via MSG_EXEC (so
// the inventory-skew "optimization" reacts on hardware), and tracks live PnL.
//
//   per tick M:  DEL prev bid/ask  ->  ADD bid@M + ADD ask@M  (book mid = M)
//                -> read OUCH quote (bid_q,ask_q) + latency (should be ~20 cyc)
//                -> host maker fill of the PREVIOUS resting quote vs this M
//                -> on fill, feed MSG_EXEC so the FPGA inventory + skew update
//   prints:  STEP <session> <i> <mid> <bid> <ask> <pnl> <fills> <inv> <lat>
// Loops forever with a fresh feed (seed+session) until the client disconnects.
//   usage: eurusd_stream <slot> <half_spread_pts> <inv_skew_q31> <seed>
// Prices are EUR/USD in 1e-5 "points" (1.08500 -> 108500; 1 pip = 10 pts).
// Register/frame layout copied verbatim from mmx_dataplane_test.cpp.
// =============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "fpga_pci.h"
#include "fpga_mgmt.h"
#include "cl_axil_reg_access_def.h"

#define ADDR_PB_ADDR 0x00
#define ADDR_PB_DATA 0x04
#define ADDR_PB_COMMIT 0x08
#define ADDR_RX_WADDR 0x0C
#define ADDR_RX_DATA_LO 0x10
#define ADDR_RX_DATA_HI 0x14
#define ADDR_RX_KEEP 0x18
#define ADDR_RX_NBEATS 0x1C
#define ADDR_RX_GO 0x20
#define ADDR_TX_COUNT 0x24
#define ADDR_TX_DATA_LO 0x2C
#define ADDR_TX_DATA_HI 0x30
#define ADDR_SESSION 0x38
#define ADDR_UDP_PORT 0x3C
#define ADDR_QCPU 0x40
#define ADDR_LOADERR 0x44
#define ADDR_LATENCY 0x48
#define ADDR_STATUS 0x4C
#define ADDR_RESET 0x50
#define ADDR_BOOK_CLEAR 0x54
#define ADDR_DBG_QC 0x58
#define STATUS_TX_DONE 0x4u
#define STATUS_REPLAY_BUSY 0x2u

#define MSG_ADD 0x41
#define MSG_DEL 0x44
#define MSG_EXEC 0x45
#define SIDE_BID 0x00
#define SIDE_ASK 0x01
static const uint16_t UDP_PORT = 0x1F90;
static const uint32_t SESSION  = 0xDEADBEEFu;

static pci_bar_handle_t g_h = PCI_BAR_HANDLE_INIT;
static void wr(uint32_t a,uint32_t v){ fpga_pci_poke(g_h,a,v); }
static uint32_t rd(uint32_t a){ uint32_t v=0; fpga_pci_peek(g_h,a,&v); return v; }

static void load_itch(uint8_t mt,uint8_t side,uint16_t tk,int32_t price,uint32_t size){
    uint8_t f[64]; memset(f,0,sizeof(f));
    f[12]=0x08; f[13]=0x00; f[14]=0x45; f[23]=0x11;
    f[36]=(uint8_t)(UDP_PORT>>8); f[37]=(uint8_t)(UDP_PORT&0xFF);
    uint8_t*pl=&f[42];
    pl[0]=mt; pl[9]=(uint8_t)(tk>>8); pl[10]=(uint8_t)(tk&0xFF); pl[11]=side;
    pl[12]=(uint8_t)((uint32_t)price>>24); pl[13]=(uint8_t)((uint32_t)price>>16);
    pl[14]=(uint8_t)((uint32_t)price>>8);  pl[15]=(uint8_t)((uint32_t)price);
    pl[16]=(uint8_t)(size>>24); pl[17]=(uint8_t)(size>>16); pl[18]=(uint8_t)(size>>8); pl[19]=(uint8_t)(size);
    for(int k=0;k<8;k++){
        uint32_t lo=f[k*8]|(f[k*8+1]<<8)|(f[k*8+2]<<16)|((uint32_t)f[k*8+3]<<24);
        uint32_t hi=f[k*8+4]|(f[k*8+5]<<8)|(f[k*8+6]<<16)|((uint32_t)f[k*8+7]<<24);
        uint32_t keep=((k<7)?0xFF:0x3F)|((k==7)?0x100:0);
        wr(ADDR_RX_WADDR,k); wr(ADDR_RX_DATA_LO,lo); wr(ADDR_RX_DATA_HI,hi); wr(ADDR_RX_KEEP,keep);
    }
    wr(ADDR_RX_NBEATS,8);
}
// fire one frame through the pipeline; optionally read back the emitted quote
static void fire(int read_q,int32_t*bid,int32_t*ask,uint32_t*lat){
    // wait until any previous replay has fully drained (else RX_GO is ignored -> msg dropped)
    for(int i=0;i<5000;i++){ if(!(rd(ADDR_STATUS)&STATUS_REPLAY_BUSY)) break; usleep(2); }
    wr(ADDR_RX_GO,1);
    uint32_t st=0; for(int i=0;i<8000;i++){ st=rd(ADDR_STATUS); if(st&STATUS_TX_DONE) break; usleep(2); }
    // ensure the frame was fully consumed before the caller fires the next one
    for(int i=0;i<3000;i++){ if(!(rd(ADDR_STATUS)&STATUS_REPLAY_BUSY)) break; usleep(2); }
    if(read_q){
        wr(ADDR_RX_WADDR,2);            // beat 2 = bid:ask
        *bid=(int32_t)rd(ADDR_TX_DATA_HI); *ask=(int32_t)rd(ADDR_TX_DATA_LO);
        *lat=rd(ADDR_LATENCY);
    }
}
static void set_params(int hs,int isk){
    uint32_t w[16]; memset(w,0,sizeof(w));
    w[0]=(uint32_t)isk;      // inv_skew_q31
    w[1]=(uint32_t)hs;       // half_spread
    w[2]=100;                // max_inv
    w[3]=1;                  // base_qty
    w[4]=0;                  // {bid_skew,kill,regime}=0
    w[5]=0x00010000;         // requote_ticks=1, ask_skew=0
    // w6..w15 = 0 (epoch=0, reserved=0, tag ignored: no-HMAC AFI)
    for(int i=0;i<16;i++){ wr(ADDR_PB_ADDR,i); wr(ADDR_PB_DATA,w[i]); }
    wr(ADDR_PB_COMMIT,1); usleep(3000);
}

int main(int argc,char**argv){
    signal(SIGPIPE,SIG_DFL);
    int slot=(argc>1)?atoi(argv[1]):0;
    int hs=(argc>2)?atoi(argv[2]):15;
    int isk=(argc>3)?atoi(argv[3]):0;
    unsigned seed=(argc>4)?(unsigned)atoi(argv[4]):1;
    if(fpga_mgmt_init()){ fprintf(stderr,"mgmt init failed\n"); return 1; }
    if(fpga_pci_attach(slot,CL_AXIL_DEMO_APP_PF,CL_AXIL_DEMO_BAR_ID,CL_AXIL_DEMO_PCI_FLAGS,&g_h)){
        fprintf(stderr,"attach failed (data-plane AFI loaded on slot %d?)\n",slot); return 1; }
    wr(ADDR_UDP_PORT,UDP_PORT); wr(ADDR_SESSION,SESSION);
    printf("EURUSD stream: half_spread=%d pts inv_skew=%d (EUR/USD in 1e-5 points, 1 pip=10)\n",hs,isk);
    fflush(stdout);

    // one continuous feed, forever. Book-clear per tick (empties order book but
    // KEEPS on-chip inventory + params) -> mid tracks M AND the A-S inventory-skew
    // persists/accumulates -> continuous optimization on real silicon.
    wr(ADDR_RESET,1); usleep(1000); set_params(hs,isk);
    printf("SESSION 0 load_err=%u\n", rd(ADDR_LOADERR)&3); fflush(stdout);
    srand(seed);
    int mid=108500, have=0, pbid_q=0, pask_q=0;
    long long cash=0; int inv=0; unsigned fills=0;
    for(int i=0;;i++){
        int drift=(108500-mid)/40, nz=(rand()%61)-30; mid+=drift+nz; if(mid<100000)mid=100000;
        int filled=0, fbuy=0;
        if(have){
            if(mid<=pbid_q && inv<100){ inv++; cash-=pbid_q; fills++; filled=1; fbuy=1; }
            else if(mid>=pask_q && inv>-100){ inv--; cash+=pask_q; fills++; filled=1; fbuy=0; }
        }
        // clear ONLY the book (inventory + params persist), rebuild 2-sided book at mid=M
        wr(ADDR_BOOK_CLEAR,1); usleep(1500);
        load_itch(MSG_ADD,SIDE_BID,1,mid-1,1000); fire(0,0,0,0);
        load_itch(MSG_ADD,SIDE_ASK,1,mid+1,1000); fire(0,0,0,0);
        // report our fill to the FPGA -> on-chip inventory + skew update (found at a book level)
        if(filled){
            if(fbuy) load_itch(MSG_EXEC,SIDE_BID,1,mid-1,1); else load_itch(MSG_EXEC,SIDE_ASK,1,mid+1,1);
            fire(0,0,0,0);
        }
        // touch to emit a fresh quote at the current inventory (skew applied), read it
        int32_t bid=0,ask=0; uint32_t lat=0;
        load_itch(MSG_ADD,SIDE_ASK,1,mid+1,1); fire(1,&bid,&ask,&lat);
        int qcpu=(int)rd(ADDR_QCPU), qc=(int)rd(ADDR_DBG_QC);
        long long pnl=cash+(long long)inv*mid;
        printf("STEP 0 %d %d %d %d %lld %u %d %u %d %d\n", i,mid,bid,ask,pnl,fills,inv,lat,qcpu,qc);
        if(fflush(stdout)!=0) return 0;
        pbid_q=bid; pask_q=ask; have=1;
        usleep(50000);
    }
}
