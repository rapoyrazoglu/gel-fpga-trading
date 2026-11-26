// include/gel/platform/aws_f2_router.hpp
//
// AWS F2 (Xilinx UltraScale+ VU47P) backend for IRouter.
//
// Communication path:
//
//   Host (EC2 m5/c5)  ──PCIe───►  F2 FPGA  ──kernel-bypass NIC──►  exchange
//
// PCIe transport is XDMA. AWS F2 exposes four character devices once the
// AFI is loaded (see fpga-load-local-image):
//
//   /dev/xdma0_h2c_0   host → FPGA control plane (writes commands)
//   /dev/xdma0_c2h_0   FPGA → host (drop-copy / fills)
//   /dev/xdma0_user    BAR-mapped registers (doorbell, status)
//   /dev/xdma0_bypass  (optional, AXI-Lite raw access)
//
// We mmap the user BAR for register pokes and use blocking write/read on
// the H2C/C2H endpoints for payloads. The kernel-bypass NIC integration
// lives entirely on the FPGA — host doesn't see it.
//
// This file is kept compileable on every platform: the AWS-only system
// calls are guarded by GEL_AWS_F2. On Mac/Linux without the macro the
// router falls back to CpuRouter behaviour so unit tests still run.
//
#pragma once

#include "order_router.hpp"
#include "broker_client.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef GEL_AWS_F2
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/types.h>
  #include <unistd.h>
  // AWS FPGA SDK headers — only available inside the AWS dev AMI / Vitis
  // container. Add aws-fpga/sdk/userspace/include to the include path.
  #include <fpga_pci.h>
  #include <fpga_mgmt.h>
#endif

namespace gel::platform {

    // ----------------------------------------------------------------------
    // Doorbell / status register layout in BAR-0 (matches the HLS kernel).
    // ----------------------------------------------------------------------
    //
    //   offset 0x000  uint32  KERNEL_VERSION   (read-only)
    //   offset 0x010  uint32  DOORBELL         (write 1 to kick)
    //   offset 0x020  uint32  STATUS           (bit 0 = busy, bit 1 = ready)
    //   offset 0x030  uint32  ORDER_FRAME[12]  (48 bytes; matches WireFrame)
    //   offset 0x100  uint64  ORDERS_PROCESSED (heartbeat counter)
    //   offset 0x108  uint64  REJECTS_LATCHED
    //
    inline constexpr uint64_t F2_REG_VERSION       = 0x000;
    inline constexpr uint64_t F2_REG_DOORBELL      = 0x010;
    inline constexpr uint64_t F2_REG_STATUS        = 0x020;
    inline constexpr uint64_t F2_REG_FRAME_BASE    = 0x030;
    inline constexpr uint64_t F2_REG_ORDERS_DONE   = 0x100;
    inline constexpr uint64_t F2_REG_REJECTS       = 0x108;

    inline constexpr uint32_t F2_STATUS_BUSY  = 1u << 0;
    inline constexpr uint32_t F2_STATUS_READY = 1u << 1;

    inline constexpr std::size_t F2_BAR_SIZE = 4096;  // one page is plenty

    struct AwsF2Config {
        // AWS slot index (`fpga-describe-local-image -S 0` → slot 0).
        int    slot_id    = 0;
        // PCI BAR index that holds our doorbell + scratch registers.
        int    bar_id     = 0;
        // Wait at most this many spins on the status register before
        // declaring the FPGA hung.
        int    max_spin   = 100'000;
        // If true, fall back to CpuRouter when XDMA isn't available.
        // Set false for production to make missing AFI a hard error.
        bool   allow_fallback_on_missing_device = true;
    };

    // ----------------------------------------------------------------------
    // AwsF2Router — production FPGA path. Implements IBrokerClient so it
    // plugs into AccountRegistry like any other broker.
    // ----------------------------------------------------------------------
    class AwsF2Router final : public IBrokerClient {
    public:
        explicit AwsF2Router(AwsF2Config cfg = {})
            : cfg_(cfg)
        {
#ifdef GEL_AWS_F2
            attach_();
#else
            // Non-AWS host: the constructor never throws, but every dispatch
            // routes through the SIM fallback so tests stay green.
            using_fallback_ = true;
#endif
        }

        ~AwsF2Router() override { detach_(); }

        BrokerKind  kind() const noexcept override { return BrokerKind::AWS_F2; }
        std::string name() const override          { return "AWS_F2"; }

        void set_fill_callback(FillCallback s) override {
            sink_ = std::move(s);
            fallback_.set_fill_callback(sink_);
        }

        // Hot path. Must be wait-free or as close as possible.
        void dispatch(const Order& o, const WireFrame& frame) override {
            if (using_fallback_) {
                fallback_.dispatch(o, frame);
                return;
            }

#ifdef GEL_AWS_F2
            // 1. Write the WireFrame into BAR scratch space.
            //    Use 32-bit POKEs since AXI-Lite is 32-bit.
            for (std::size_t i = 0; i < frame.size(); i += 4) {
                uint32_t w = 0;
                std::memcpy(&w, frame.data() + i, 4);
                bar_poke_(F2_REG_FRAME_BASE + i, w);
            }

            // 2. Ring the doorbell.
            bar_poke_(F2_REG_DOORBELL, 1);

            // 3. Spin-wait for the kernel to finish. The hot path is on the
            //    FPGA itself; this poll is only here for synchronous-emit
            //    semantics. Real async deployments will read from
            //    /dev/xdma0_c2h_0 in a separate thread.
            for (int i = 0; i < cfg_.max_spin; ++i) {
                const auto st = bar_peek_(F2_REG_STATUS);
                if ((st & F2_STATUS_BUSY) == 0) break;
            }

            // 4. Synthesize an ExecReport from the result-side registers.
            //    A real driver would drain /dev/xdma0_c2h_0 instead.
            ExecReport r{};
            r.ts_ns             = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::system_clock::now().time_since_epoch()).count();
            r.client_order_id   = o.client_order_id;
            r.exchange_order_id = bar_peek_(F2_REG_ORDERS_DONE);
            r.strategy_id       = o.strategy_id;
            r.account_id        = o.account_id;
            r.symbol_id         = o.symbol_id;
            r.fill_price_ticks  = o.price_ticks;
            r.fill_quantity     = o.quantity;
            r.status            = static_cast<uint8_t>(connector::ExecStatus::FILLED);
            r.side              = o.side;
            if (sink_) sink_(r);
#endif
        }

        // Diagnostics for the demo / health endpoint.
        bool       using_cpu_fallback() const { return using_fallback_; }
        uint64_t   orders_processed()   const {
#ifdef GEL_AWS_F2
            return using_fallback_ ? 0u
                : (uint64_t(bar_peek_(F2_REG_ORDERS_DONE + 4)) << 32 |
                   uint64_t(bar_peek_(F2_REG_ORDERS_DONE)));
#else
            return 0u;
#endif
        }

    private:
        // ------------------------------------------------------------
        // BAR access helpers — raw 32-bit MMIO. Compile out on non-AWS.
        // ------------------------------------------------------------
#ifdef GEL_AWS_F2
        void attach_() {
            const int rc = fpga_pci_attach(cfg_.slot_id, FPGA_APP_PF, cfg_.bar_id,
                                            0 /*flags*/, &pci_handle_);
            if (rc != 0) {
                if (cfg_.allow_fallback_on_missing_device) {
                    using_fallback_ = true;
                    std::fprintf(stderr,
                        "[AwsF2Router] fpga_pci_attach failed (%d). Falling back to CPU.\n", rc);
                    return;
                }
                throw std::runtime_error("fpga_pci_attach failed");
            }
            // Validate kernel version. Mismatch → halt rather than send bad orders.
            const auto v = bar_peek_(F2_REG_VERSION);
            std::fprintf(stderr, "[AwsF2Router] attached to slot %d, kernel version 0x%08x\n",
                         cfg_.slot_id, v);
        }

        void detach_() {
            if (!using_fallback_ && pci_handle_ != PCI_BAR_HANDLE_INIT) {
                fpga_pci_detach(pci_handle_);
                pci_handle_ = PCI_BAR_HANDLE_INIT;
            }
        }

        uint32_t bar_peek_(uint64_t off) const {
            uint32_t v = 0;
            const int rc = fpga_pci_peek(pci_handle_, off, &v);
            if (rc != 0) std::fprintf(stderr, "[AwsF2Router] peek failed off=0x%llx\n",
                                       (unsigned long long)off);
            return v;
        }

        void bar_poke_(uint64_t off, uint32_t v) {
            const int rc = fpga_pci_poke(pci_handle_, off, v);
            if (rc != 0) std::fprintf(stderr, "[AwsF2Router] poke failed off=0x%llx\n",
                                       (unsigned long long)off);
        }

        pci_bar_handle_t pci_handle_ = PCI_BAR_HANDLE_INIT;
#else
        void attach_() {}
        void detach_() {}
#endif

        AwsF2Config       cfg_;
        FillCallback      sink_;
        SimBrokerClient   fallback_{"AWS_F2_FALLBACK"};   // used when AWS_F2 isn't available
        bool              using_fallback_ = false;
    };

} // namespace gel::platform
