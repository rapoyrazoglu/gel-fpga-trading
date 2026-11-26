// mmx_fpga_test_v3.c — on-silicon validation of the SMART pricing engine.
// Part 1: OBI-enhanced pricing parity (500 vectors) vs C++ reference.
// Part 2: on-chip measured latency.  Part 3: volatility-adaptive spread.
// Part 4: hardware circuit breaker (cap + pnl trips).
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "fpga_pci.h"
#include "fpga_mgmt.h"
#include "cl_axil_reg_access_def.h"

#define A_S 0x00
#define A_Q 0x04
#define A_INV 0x08
#define A_HS 0x0C
#define A_MAX 0x10
#define A_BQ 0x14
#define A_SKEWS 0x18
#define A_KILL 0x1C
#define A_BID 0x24
#define A_ASK 0x28
#define A_QTY 0x2C
#define A_CANCEL 0x30
#define A_IMB 0x34
#define A_OBIG 0x38
#define A_KSH 0x3C
#define A_VOLG 0x40
#define A_PNL 0x44
#define A_CAPT 0x48
#define A_PNLD 0x4C
#define A_RST 0x50
#define A_SPRD 0x54
#define A_VOLO 0x58
#define A_HK 0x5C
#define A_TRIP 0x60
#define A_LAT 0x64

static pci_bar_handle_t h;
static void wr(uint32_t a,uint32_t v){ fpga_pci_poke(h,a,v); }
static uint32_t rd(uint32_t a){ uint32_t v=0; fpga_pci_peek(h,a,&v); return v; }

static int32_t sat32(int64_t v){ if(v>2147483647LL)return 2147483647; if(v<-2147483648LL)return -2147483648; return (int32_t)v; }
static void compute_quote(int32_t s,int32_t q,int32_t inv,int32_t hs,uint32_t mx,uint32_t bq,
    int16_t bsk,int16_t ask_sk,uint8_t kill,int32_t*bid,int32_t*ask,uint32_t*qty,uint8_t*can){
    int64_t cap=(int64_t)mx; uint8_t k=kill|(((int64_t)q>cap)||((int64_t)q<-cap));
    int64_t qc=q; if(qc>cap)qc=cap; if(qc<-cap)qc=-cap;
    int64_t prod=qc*(int64_t)inv, skew=(prod+(1LL<<30))>>31, r=(int64_t)s-skew;
    *bid=sat32(r-(int64_t)hs-(int64_t)bsk); *ask=sat32(r+(int64_t)hs+(int64_t)ask_sk); *qty=bq; *can=k;
}

int main(int argc,char**argv){
    const char*vf=(argc>1)?argv[1]:"pricing_vectors_v3.txt";
    if(fpga_mgmt_init()){fprintf(stderr,"mgmt init fail\n");return 1;}
    if(fpga_pci_attach(0,CL_AXIL_DEMO_APP_PF,CL_AXIL_DEMO_BAR_ID,CL_AXIL_DEMO_PCI_FLAGS,&h)){
        fprintf(stderr,"attach fail (AFI loaded?)\n");return 1;}

    // ---- Part 1: OBI-enhanced pricing parity (vol off, breaker disabled) ----
    wr(A_KSH,6); wr(A_VOLG,0); wr(A_CAPT,200); wr(A_PNLD,0x7FFFFFFF); wr(A_PNL,0);
    FILE*f=fopen(vf,"r"); int n=0; if(!f||fscanf(f,"%d",&n)!=1){fprintf(stderr,"vec file?\n");return 1;}
    int mism=0;
    for(int i=0;i<n;i++){
        int s,q,inv,hs,mx,bq,bsk,ask_sk,kl,imb,gain,ebid,eask,eqty,ecan;
        if(fscanf(f,"%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
            &s,&q,&inv,&hs,&mx,&bq,&bsk,&ask_sk,&kl,&imb,&gain,&ebid,&eask,&eqty,&ecan)!=15) break;
        wr(A_RST,1);
        wr(A_S,s); wr(A_Q,q); wr(A_INV,inv); wr(A_HS,hs); wr(A_MAX,mx); wr(A_BQ,bq);
        wr(A_SKEWS, ((uint32_t)(uint16_t)ask_sk<<16)|(uint16_t)bsk); wr(A_KILL,kl&1);
        wr(A_IMB,(uint16_t)imb); wr(A_OBIG,(uint16_t)gain);
        uint32_t bid=rd(A_BID),ask=rd(A_ASK),qty=rd(A_QTY),can=rd(A_CANCEL);
        if((int32_t)bid!=ebid||(int32_t)ask!=eask||qty!=(uint32_t)eqty||(can&1)!=(ecan&1)){
            if(mism<5) printf("  MISMATCH %d: FPGA %d/%d host %d/%d\n",i,(int32_t)bid,(int32_t)ask,ebid,eask);
            mism++;
        }
    }
    fclose(f);
    printf("Part1 OBI parity: %d vectors, %d mismatches -> %s\n",n,mism,mism==0?"BIT-EXACT":"FAIL");

    // ---- Part 2: on-chip measured pricing latency ----
    wr(A_S, 10000);  // one tick -> triggers the tag/latency counter
    uint32_t lat=rd(A_LAT);
    printf("Part2 latency: pipeline = %u cycles = %u ns @250MHz\n", lat, lat*4);

    // ---- Part 3: volatility-adaptive spread (fixed decay lambda=1-2^-6) ----
    wr(A_VOLG, 20000);
    for(int i=0;i<400;i++){ wr(A_S, 10000); }          // flat ticks -> EWMA decays to ~0
    uint32_t sp_calm=rd(A_SPRD);
    for(int i=0;i<60;i++){ wr(A_S, 10000 + ((i&1)?4000:-4000)); }  // volatile: +/-4000
    uint32_t sp_vol=rd(A_SPRD); uint32_t volo=rd(A_VOLO);
    printf("Part3 vol-adaptive: spread_add calm=%u volatile=%u (vol_out=%u) -> %s\n",
           sp_calm,sp_vol,volo, sp_vol>sp_calm?"WIDENS ON VOL":"no response");
    wr(A_VOLG,0);

    // ---- Part 4: hardware circuit breaker ----
    wr(A_RST,1); wr(A_MAX,100); wr(A_CAPT,5);
    for(int i=0;i<8;i++){ wr(A_Q, 500); wr(A_Q, 0); }   // 8 rising cap breaches (>=5)
    uint32_t hk_cap=rd(A_HK), tr_cap=rd(A_TRIP);
    wr(A_RST,1); wr(A_CAPT,200); wr(A_PNLD,1000); wr(A_PNL,(uint32_t)(-5000));
    uint32_t hk_pnl=rd(A_HK), tr_pnl=rd(A_TRIP);
    printf("Part4 breaker: cap-trip hk=%u reason=%u ; pnl-trip hk=%u reason=%u -> %s\n",
           hk_cap,tr_cap,hk_pnl,tr_pnl, (hk_cap&&hk_pnl)?"BOTH TRIP":"FAIL");

    printf("\n=== SMART PRICING ON SILICON: parity=%s latency=%uc vol=%s breaker=%s ===\n",
           mism==0?"OK":"FAIL", lat, sp_vol>sp_calm?"OK":"?", (hk_cap&&hk_pnl)?"OK":"FAIL");
    fpga_pci_detach(h);
    return 0;
}
