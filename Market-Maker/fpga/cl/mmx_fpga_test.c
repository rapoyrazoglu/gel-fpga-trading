// mmx_fpga_test.c — on-silicon parity check for the MMX pricing engine.
// Drives the CL over the OCL AXI-Lite BAR: writes (s, q, ParamBlock) registers,
// reads back (bid, ask, qty, cancel), and compares to the host compute_quote()
// over the same 500 vectors used in RTL simulation. 0 mismatches == the FPGA
// computes bit-exact quotes on real hardware.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "fpga_pci.h"
#include "fpga_mgmt.h"
#include "cl_axil_reg_access_def.h"

// ---- MMX register map (must match cl_axil_reg_access_defines.vh) ----
#define ADDR_S            0x00
#define ADDR_Q            0x04
#define ADDR_INV_SKEW     0x08
#define ADDR_HALF_SPREAD  0x0C
#define ADDR_MAX_INV      0x10
#define ADDR_BASE_QTY     0x14
#define ADDR_SKEWS        0x18   /* [15:0]=bid_skew, [31:16]=ask_skew */
#define ADDR_KILL         0x1C
#define ADDR_BID          0x24
#define ADDR_ASK          0x28
#define ADDR_QTY          0x2C
#define ADDR_CANCEL       0x30

static int32_t sat32(int64_t v){
    if (v >  2147483647LL) return  2147483647;
    if (v < -2147483648LL) return -2147483648;
    return (int32_t)v;
}
// host reference — identical math to host/pricing/pricing_engine.h
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

int main(int argc,char**argv){
    int slot=0; const char*vf=(argc>1)?argv[1]:"pricing_vectors.txt";
    pci_bar_handle_t h=PCI_BAR_HANDLE_INIT;
    if(fpga_mgmt_init()){fprintf(stderr,"mgmt init failed\n");return 1;}
    int rc=fpga_pci_attach(slot,CL_AXIL_DEMO_APP_PF,CL_AXIL_DEMO_BAR_ID,CL_AXIL_DEMO_PCI_FLAGS,&h);
    if(rc){fprintf(stderr,"fpga_pci_attach failed rc=%d (is our AFI loaded on slot %d?)\n",rc,slot);return 1;}

    FILE*f=fopen(vf,"r"); if(!f){fprintf(stderr,"cannot open %s\n",vf);return 1;}
    int n=0; if(fscanf(f,"%d",&n)!=1){fprintf(stderr,"bad vector file\n");return 1;}
    int mism=0, shown=0;
    for(int i=0;i<n;i++){
        int s,q,inv,hs,mx,bq,bsk,ask_sk,kl,ebid,eask,eqty,ecan;
        if(fscanf(f,"%d %d %d %d %d %d %d %d %d %d %d %d %d",
            &s,&q,&inv,&hs,&mx,&bq,&bsk,&ask_sk,&kl,&ebid,&eask,&eqty,&ecan)!=13) break;
        uint32_t skews=((uint32_t)(uint16_t)ask_sk<<16)|(uint16_t)bsk;
        fpga_pci_poke(h,ADDR_S,(uint32_t)s);           fpga_pci_poke(h,ADDR_Q,(uint32_t)q);
        fpga_pci_poke(h,ADDR_INV_SKEW,(uint32_t)inv);  fpga_pci_poke(h,ADDR_HALF_SPREAD,(uint32_t)hs);
        fpga_pci_poke(h,ADDR_MAX_INV,(uint32_t)mx);    fpga_pci_poke(h,ADDR_BASE_QTY,(uint32_t)bq);
        fpga_pci_poke(h,ADDR_SKEWS,skews);             fpga_pci_poke(h,ADDR_KILL,(uint32_t)(kl&1));
        // PCIe peek round-trip (~us) >> 4-cycle (16 ns) pipeline, so the result is settled.
        uint32_t bid=0,ask=0,qty=0,can=0;
        fpga_pci_peek(h,ADDR_BID,&bid); fpga_pci_peek(h,ADDR_ASK,&ask);
        fpga_pci_peek(h,ADDR_QTY,&qty); fpga_pci_peek(h,ADDR_CANCEL,&can);

        int32_t rbid,rask; uint32_t rqty; uint8_t rcan;
        compute_quote(s,q,inv,hs,(uint32_t)mx,(uint32_t)bq,(int16_t)bsk,(int16_t)ask_sk,(uint8_t)kl,
                      &rbid,&rask,&rqty,&rcan);
        int bad = ((int32_t)bid!=rbid)||((int32_t)ask!=rask)||(qty!=rqty)||((can&1)!=(rcan&1));
        if(bad){ mism++; if(shown++<5)
            printf("MISMATCH vec %d: FPGA bid=%d ask=%d  host bid=%d ask=%d\n",
                   i,(int32_t)bid,(int32_t)ask,rbid,rask); }
        if(i<3) printf("vec %d: s=%d q=%d -> FPGA bid=%d ask=%d qty=%u cancel=%u (host bid=%d ask=%d)\n",
                       i,s,q,(int32_t)bid,(int32_t)ask,qty,can&1,rbid,rask);
    }
    fclose(f);
    printf("\n=== ON-SILICON PARITY: %d vectors, %d mismatches -> %s ===\n",
           n,mism, mism==0?"BIT-EXACT ON HARDWARE":"MISMATCH");
    fpga_pci_detach(h);
    return mism==0?0:1;
}
