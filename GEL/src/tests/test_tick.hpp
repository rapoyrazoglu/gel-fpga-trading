// src/tests/test_tick.hpp
#pragma once

#include <iostream>
#include <iomanip>
#include "gel/data/tick.hpp"
#include "gel/data/datetime_utils.hpp"

namespace gel::test {

void test_tick_conversion() {
    std::cout << "Testing Tick CPU <-> FPGA conversion\n";

    // Test verileri
    gel::data::Tick tick;
    tick.timestamp = gel::data::DateTimeUtils::parseISO8601("2003-05-05 00:00:05.536000+00:00");
    tick.bid = 1.12161;
    tick.ask = 1.12177;
    tick.bid_volume = 14.4;
    tick.ask_volume = 35.7;

    std::cout << "Original Tick:\n";
    std::cout << "Timestamp: " << gel::data::DateTimeUtils::toISO8601(tick.timestamp) << "\n";
    std::cout << "Bid: " << tick.bid << ", Ask: " << tick.ask << "\n";
    std::cout << "Spread: " << tick.spread() << ", Mid: " << tick.mid_price() << "\n";
    std::cout << "Bid Volume: " << tick.bid_volume << ", Ask Volume: " << tick.ask_volume << "\n";

    // CPU -> FPGA
    auto fpga_tick = tick.toFPGA();

    // FPGA -> CPU
    auto converted_tick = gel::data::Tick::fromFPGA(fpga_tick);

    std::cout << "\nConverted Tick:\n";
    std::cout << "Timestamp: " << gel::data::DateTimeUtils::toISO8601(converted_tick.timestamp) << "\n";
    std::cout << "Bid: " << converted_tick.bid << ", Ask: " << converted_tick.ask << "\n";
    std::cout << "Spread: " << converted_tick.spread() << ", Mid: " << converted_tick.mid_price() << "\n";
    std::cout << "Bid Volume: " << converted_tick.bid_volume << ", Ask Volume: " << converted_tick.ask_volume << "\n";

    // Hata analizi
    double bid_error = std::abs(tick.bid - converted_tick.bid);
    double ask_error = std::abs(tick.ask - converted_tick.ask);
    double bid_vol_error = std::abs(tick.bid_volume - converted_tick.bid_volume);
    double ask_vol_error = std::abs(tick.ask_volume - converted_tick.ask_volume);

    std::cout << "\nConversion Errors:\n";
    std::cout << "Bid error: " << bid_error << " (" << bid_error / 0.0001 << " pips)\n";
    std::cout << "Ask error: " << ask_error << " (" << ask_error / 0.0001 << " pips)\n";
    std::cout << "Bid Volume error: " << bid_vol_error << "\n";
    std::cout << "Ask Volume error: " << ask_vol_error << "\n";
}

} // namespace gel::test