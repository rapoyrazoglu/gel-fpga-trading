// src/tests/test_ohlc.hpp
#pragma once

#include <iostream>
#include <iomanip>
#include <chrono>
#include "gel/data/ohlc.hpp"
#include "gel/data/dukascopy_parser.hpp"
#include "gel/data/datetime_utils.hpp"

namespace gel::test {

void test_ohlc_conversion() {
    std::cout << "Testing OHLC CPU <-> FPGA conversion\n";

    // Test verisi
    gel::data::OHLCBarBar bar;
    bar.timestamp = gel::data::DateTimeUtils::parseISO8601("2003-05-05 00:00:00.000000+00:00");
    bar.open = 1.12161;
    bar.high = 1.12183;
    bar.low = 1.12160;
    bar.close = 1.12175;
    bar.volume = 123.45;

    std::cout << "Original OHLC Bar:\n";
    std::cout << "Timestamp: " << gel::data::DateTimeUtils::toISO8601(bar.timestamp) << "\n";
    std::cout << "O: " << bar.open << ", H: " << bar.high << ", L: " << bar.low << ", C: " << bar.close << "\n";
    std::cout << "Volume: " << bar.volume << "\n";
    std::cout << "Typical Price: " << bar.get_typical_price() << "\n";

    // CPU -> FPGA
    auto fpga_bar = bar.toFPGA();

    // FPGA -> CPU
    auto converted_bar = gel::data::OHLCBarBar::fromFPGA(fpga_bar);

    std::cout << "\nConverted OHLC Bar:\n";
    std::cout << "Timestamp: " << gel::data::DateTimeUtils::toISO8601(converted_bar.timestamp) << "\n";
    std::cout << "O: " << converted_bar.open << ", H: " << converted_bar.high
              << ", L: " << converted_bar.low << ", C: " << converted_bar.close << "\n";
    std::cout << "Volume: " << converted_bar.volume << "\n";
    std::cout << "Typical Price: " << converted_bar.get_typical_price() << "\n";

    // Hata analizi
    double open_error = std::abs(bar.open - converted_bar.open);
    double high_error = std::abs(bar.high - converted_bar.high);
    double low_error = std::abs(bar.low - converted_bar.low);
    double close_error = std::abs(bar.close - converted_bar.close);
    double volume_error = std::abs(bar.volume - converted_bar.volume);

    std::cout << "\nConversion Errors:\n";
    std::cout << "Open error: " << open_error << " (" << open_error / 0.0001 << " pips)\n";
    std::cout << "High error: " << high_error << " (" << high_error / 0.0001 << " pips)\n";
    std::cout << "Low error: " << low_error << " (" << low_error / 0.0001 << " pips)\n";
    std::cout << "Close error: " << close_error << " (" << close_error / 0.0001 << " pips)\n";
    std::cout << "Volume error: " << volume_error << "\n";
}

void test_ohlc_builder() {
    std::cout << "Testing OHLC Builder\n";

    try {
        // Küçük örnek dosyayı kullan
        std::string filename = "data/EURUSD_sample.csv";

        auto ticks = gel::data::DukascopyParser::parseTickFile(filename);

        if (!ticks.empty()) {
            std::cout << "Creating OHLC bars from " << ticks.size() << " ticks\n";

            // Farklı timeframe'leri test et
            auto start = std::chrono::high_resolution_clock::now();

            auto m1_bars = gel::data::OHLCBarBuilder::fromTicks(ticks, std::chrono::minutes(1));
            auto m5_bars = gel::data::OHLCBarBuilder::fromTicks(ticks, std::chrono::minutes(5));
            auto h1_bars = gel::data::OHLCBarBuilder::fromTicks(ticks, std::chrono::minutes(60));

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

            std::cout << "Created " << m1_bars.size() << " 1-minute bars, "
                      << m5_bars.size() << " 5-minute bars, and "
                      << h1_bars.size() << " 1-hour bars in " << duration.count() << " ms\n";

            if (!m1_bars.empty()) {
                // İlk bar
                std::cout << "\nFirst 1-minute bar:\n";
                std::cout << "Time: " << gel::data::DateTimeUtils::toISO8601(m1_bars.front().timestamp) << "\n";
                std::cout << "O: " << m1_bars.front().open << ", H: " << m1_bars.front().high
                          << ", L: " << m1_bars.front().low << ", C: " << m1_bars.front().close << "\n";
                std::cout << "Volume: " << m1_bars.front().volume << "\n";

                // Son bar
                std::cout << "\nLast 1-minute bar:\n";
                std::cout << "Time: " << gel::data::DateTimeUtils::toISO8601(m1_bars.back().timestamp) << "\n";
                std::cout << "O: " << m1_bars.back().open << ", H: " << m1_bars.back().high
                          << ", L: " << m1_bars.back().low << ", C: " << m1_bars.back().close << "\n";
                std::cout << "Volume: " << m1_bars.back().volume << "\n";
            }

            // RealTimeBuilder testi (opsiyonel)
            std::cout << "\nTesting RealTimeBuilder:\n";
            gel::data::OHLCBarBuilder::RealTimeBuilder rt_builder(std::chrono::minutes(1));

            int completed_bars = 0;
            for (const auto& tick : ticks) {
                auto bar = rt_builder.addTick(tick);
                if (bar.has_value()) {
                    completed_bars++;
                    if (completed_bars <= 3) {  // Sadece ilk 3 bar'ı göster
                        std::cout << "New bar completed at " << gel::data::DateTimeUtils::toISO8601(bar->timestamp) << "\n";
                        std::cout << "O: " << bar->open << ", H: " << bar->high
                                  << ", L: " << bar->low << ", C: " << bar->close << "\n";
                    }
                }
            }

            std::cout << "Total bars created with RealTimeBuilder: " << completed_bars << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Error in OHLC Builder test: " << e.what() << std::endl;
        throw; // Test framework'ün hata yakalaması için yeniden fırlat
    }
}

} // namespace gel::test