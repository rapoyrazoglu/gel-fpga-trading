// src/tests/test_integration.hpp
#pragma once

#include <iostream>
#include <vector>
#include <algorithm>
#include "gel/data/csv_reader.hpp"
#include "gel/data/dukascopy_parser.hpp"
#include "gel/data/tick.hpp"
#include "gel/data/ohlc.hpp"
#include "gel/testing/logger.hpp"

namespace gel::test {

/**
 * @brief Integration Test: E2E Data Pipeline (CSV → Tick → OHLC)
 *
 * This test validates the data processing pipeline foundation:
 * 1. CSV reading (8.5M EUR/USD ticks)
 * 2. Tick parsing (Dukascopy format)
 * 3. OHLC building (1-minute bars from ticks)
 *
 * Pattern detection and trading logic will be added in Week 1 Day 2-3.
 */
void test_integration() {
    std::cout << "Testing Integration (E2E Data Pipeline)\n";
    LOG_INFO("Starting integration test - Data pipeline foundation");

    try {
        // Step 1: Read CSV
        std::cout << "\n=== Step 1: Reading CSV ===\n";
        const std::string filename = "data/EURUSD_all_ticks_20250601_20251014.csv";
        auto data = gel::data::CSVReader::parseCSV(filename, ',', true, 500);
        std::cout << "Rows read: " << data.size() << "\n";

        if (data.empty()) {
            throw std::runtime_error("CSV file is empty or not found");
        }

        // Step 2: Parse to Ticks
        std::cout << "\n=== Step 2: Parsing Ticks ===\n";
        std::vector<gel::data::Tick> ticks;
        for (const auto& row : data) {
            if (row.size() < 5) continue;

            gel::data::Tick tick;
            tick.timestamp = gel::data::DateTimeUtils::parseISO8601(row[0]);
            tick.bid = std::stod(row[1]);
            tick.ask = std::stod(row[2]);
            tick.bid_volume = std::stod(row[3]);
            tick.ask_volume = std::stod(row[4]);
            ticks.push_back(tick);
        }
        std::cout << "Ticks parsed: " << ticks.size() << "\n";

        if (ticks.empty()) {
            throw std::runtime_error("No ticks parsed from CSV");
        }

        // Step 3: Build OHLC
        std::cout << "\n=== Step 3: Building OHLC (1-minute bars) ===\n";
        auto ohlc_bars = gel::data::OHLCBarBuilder::fromTicks(ticks, std::chrono::minutes(1));
        std::cout << "OHLC bars: " << ohlc_bars.size() << "\n";

        if (ohlc_bars.empty() && !ticks.empty()) {
            throw std::runtime_error("OHLC builder failed to produce bars");
        }

        // Step 4: Validate OHLC data quality
        std::cout << "\n=== Step 4: Validating OHLC Quality ===\n";
        if (!ohlc_bars.empty()) {
            const auto& first_bar = ohlc_bars.front();
            const auto& last_bar = ohlc_bars.back();

            std::cout << "First bar: Open=" << first_bar.open
                      << " High=" << first_bar.high
                      << " Low=" << first_bar.low
                      << " Close=" << first_bar.close << "\n";

            std::cout << "Last bar:  Open=" << last_bar.open
                      << " High=" << last_bar.high
                      << " Low=" << last_bar.low
                      << " Close=" << last_bar.close << "\n";

            // Basic sanity checks
            for (const auto& bar : ohlc_bars) {
                if (bar.high < bar.low) {
                    throw std::runtime_error("Invalid OHLC: high < low");
                }
                if (bar.open > bar.high || bar.open < bar.low) {
                    throw std::runtime_error("Invalid OHLC: open out of range");
                }
                if (bar.close > bar.high || bar.close < bar.low) {
                    throw std::runtime_error("Invalid OHLC: close out of range");
                }
            }
            std::cout << "All OHLC bars passed quality checks\n";
        }

        std::cout << "\n✅ Integration test PASSED\n";
        std::cout << "Data pipeline ready for pattern detection (Week 1 Day 2-3)\n";
        LOG_INFO("Integration test completed - Foundation validated");

    } catch (const std::exception& e) {
        LOG_ERROR("Integration test failed: " + std::string(e.what()));
        throw;
    }
}

} // namespace gel::test
