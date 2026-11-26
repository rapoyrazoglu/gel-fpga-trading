// include/gel/data/tick.hpp
#pragma once

#include <chrono>
#include <string>
#include "fixed_point.hpp"

namespace gel::data {

    struct Tick {
        // Dukascopy ISO 8601 formatındaki datetime için
        std::chrono::system_clock::time_point timestamp;
        double bid;
        double ask;
        double bid_volume;
        double ask_volume;

        // Hesaplanan değerler
        [[nodiscard]] double spread() const { return ask - bid; }
        [[nodiscard]] double mid_price() const { return (bid + ask) / 2.0; }

        // FPGA uyumlu yapı
        struct FPGATick {
            uint64_t timestamp;  // Epoch milisaniye olarak
            fpga::FixedPoint<4, 20> bid;  // 4 bit tamsayı, 20 bit kesirli
            fpga::FixedPoint<4, 20> ask;  // 4 bit tamsayı, 20 bit kesirli
            fpga::FixedPoint<12, 20> bid_volume;  // 12 bit tamsayı, 20 bit kesirli
            fpga::FixedPoint<12, 20> ask_volume;  // 12 bit tamsayı, 20 bit kesirli

            [[nodiscard]] fpga::FixedPoint<4, 20> spread() const { return ask - bid; }
            [[nodiscard]] fpga::FixedPoint<4, 20> mid_price() const {
                return (bid + ask) / fpga::FixedPoint<4, 20>(2.0);
            }
        };

        // CPU -> FPGA dönüşümü
        [[nodiscard]] FPGATick toFPGA() const {
            FPGATick fpga_tick;

            // Timestamp: system_clock -> epoch milliseconds
            auto duration = timestamp.time_since_epoch();
            fpga_tick.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

            // Fiyatlar: double -> FixedPoint
            fpga_tick.bid = fpga::FixedPoint<4, 20>(bid);
            fpga_tick.ask = fpga::FixedPoint<4, 20>(ask);
            fpga_tick.bid_volume = fpga::FixedPoint<12, 20>(bid_volume);
            fpga_tick.ask_volume = fpga::FixedPoint<12, 20>(ask_volume);

            return fpga_tick;
        }

        // FPGA -> CPU dönüşümü
        static Tick fromFPGA(const FPGATick& fpga_tick) {
            Tick tick;

            // Timestamp: epoch milliseconds -> system_clock
            tick.timestamp = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(fpga_tick.timestamp)
            );

            // Fiyatlar: FixedPoint -> double
            tick.bid = static_cast<double>(fpga_tick.bid);
            tick.ask = static_cast<double>(fpga_tick.ask);
            tick.bid_volume = static_cast<double>(fpga_tick.bid_volume);
            tick.ask_volume = static_cast<double>(fpga_tick.ask_volume);

            return tick;
        }
    };

} // namespace gel::data