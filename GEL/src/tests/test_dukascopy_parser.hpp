// src/tests/test_dukascopy_parser.hpp
#pragma once

#include <iostream>
#include <chrono>
#include "gel/data/dukascopy_parser.hpp"
#include "gel/data/datetime_utils.hpp"

namespace gel::test {

void test_dukascopy_parser() {
    std::cout << "Testing Dukascopy Parser\n";

    try {
        // Küçük örnek dosyayı kullan
        std::string filename = "data/EURUSD_sample.csv";

        auto start = std::chrono::high_resolution_clock::now();
        auto ticks = gel::data::DukascopyParser::parseTickFile(filename);
        auto end = std::chrono::high_resolution_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "Parsed " << ticks.size() << " ticks in " << duration.count() << " ms\n";

        if (!ticks.empty()) {
            // İlk tick
            std::cout << "\nFirst tick:\n";
            std::cout << "Time: " << gel::data::DateTimeUtils::toISO8601(ticks.front().timestamp) << "\n";
            std::cout << "Bid: " << ticks.front().bid << ", Ask: " << ticks.front().ask << "\n";
            std::cout << "Bid Volume: " << ticks.front().bid_volume << ", Ask Volume: " << ticks.front().ask_volume << "\n";

            // Son tick
            std::cout << "\nLast tick:\n";
            std::cout << "Time: " << gel::data::DateTimeUtils::toISO8601(ticks.back().timestamp) << "\n";
            std::cout << "Bid: " << ticks.back().bid << ", Ask: " << ticks.back().ask << "\n";
            std::cout << "Bid Volume: " << ticks.back().bid_volume << ", Ask Volume: " << ticks.back().ask_volume << "\n";

            // Batch processing test (isteğe bağlı)
            std::cout << "\nBatch processing test:\n";
            int total_batches = 0;
            int total_ticks = 0;

            gel::data::DukascopyParser::parseTicksInBatches(filename, 1000,
                [&](const std::vector<gel::data::Tick>& batch) {
                    total_batches++;
                    total_ticks += batch.size();
                    std::cout << "Batch " << total_batches << ": " << batch.size() << " ticks\n";
                });

            std::cout << "Total batches: " << total_batches << ", total ticks: " << total_ticks << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Error parsing Dukascopy file: " << e.what() << std::endl;
        throw; // Test framework'ün hata yakalaması için yeniden fırlat
    }
}

} // namespace gel::test