// src/tests/test_fixed_point.hpp
#pragma once

#include <iostream>
#include <iomanip>
#include <vector>
#include <cmath>
#include <limits>
#include "fixed_point.hpp"

namespace gel::test {

void test_fixed_point() {
    std::cout << "Testing different Fixed-Point configurations for EUR/USD\n";

    // Dukascopy verisinden EUR/USD değer aralığı: 1.07606 - 1.24688
    double min_price = 1.07;
    double max_price = 1.25;
    double pip = 0.0001;  // 1 pip EUR/USD için

    // Farklı bit genişliklerini test et
    using FP2_14 = gel::fpga::FixedPoint<2, 14>;  // Analiz önerisi
    using FP4_20 = gel::fpga::FixedPoint<4, 20>;  // Daha geniş aralık
    using FP8_24 = gel::fpga::FixedPoint<8, 24>;  // Daha da geniş aralık

    std::vector<double> test_prices = {
        1.07606, 1.10000, 1.15000, 1.20000, 1.24688,  // Fiyat aralığı
        1.15001, 1.15002, 1.15003, 1.15004, 1.15005   // Pip hassasiyeti
    };

    std::cout << "Price\tFloat\tFP2_14\tFP4_20\tFP8_24\tFP2_14 Err (pips)\tFP4_20 Err (pips)\tFP8_24 Err (pips)\n";
    std::cout << "-----\t-----\t------\t------\t------\t----------------\t----------------\t----------------\n";

    for (double price : test_prices) {
        FP2_14 fp2_14(price);
        FP4_20 fp4_20(price);
        FP8_24 fp8_24(price);

        double fp2_14_val = static_cast<double>(fp2_14);
        double fp4_20_val = static_cast<double>(fp4_20);
        double fp8_24_val = static_cast<double>(fp8_24);

        // Hata pip olarak hesapla
        double err2_14_pips = std::abs(fp2_14_val - price) / pip;
        double err4_20_pips = std::abs(fp4_20_val - price) / pip;
        double err8_24_pips = std::abs(fp8_24_val - price) / pip;

        std::cout << std::fixed << std::setprecision(6);
        std::cout << price << "\t"
                  << price << "\t"
                  << fp2_14_val << "\t"
                  << fp4_20_val << "\t"
                  << fp8_24_val << "\t"
                  << err2_14_pips << "\t\t"
                  << err4_20_pips << "\t\t"
                  << err8_24_pips << "\n";
    }

    // Aralık test et
    std::cout << "\n=== Range Test ===\n";
    std::cout << "Configuration\tMin Value\tMax Value\tRange (pips)\n";

    FP2_14 fp2_14_min(0.0), fp2_14_max(0.0);
    FP4_20 fp4_20_min(0.0), fp4_20_max(0.0);
    FP8_24 fp8_24_min(0.0), fp8_24_max(0.0);

    fp2_14_min = FP2_14::from_raw(std::numeric_limits<int16_t>::min());
    fp2_14_max = FP2_14::from_raw(std::numeric_limits<int16_t>::max());

    fp4_20_min = FP4_20::from_raw(std::numeric_limits<int32_t>::min());
    fp4_20_max = FP4_20::from_raw(std::numeric_limits<int32_t>::max());

    fp8_24_min = FP8_24::from_raw(std::numeric_limits<int32_t>::min());
    fp8_24_max = FP8_24::from_raw(std::numeric_limits<int32_t>::max());

    double range2_14 = static_cast<double>(fp2_14_max - fp2_14_min) / pip;
    double range4_20 = static_cast<double>(fp4_20_max - fp4_20_min) / pip;
    double range8_24 = static_cast<double>(fp8_24_max - fp8_24_min) / pip;

    std::cout << "FP2_14\t\t" << static_cast<double>(fp2_14_min) << "\t" << static_cast<double>(fp2_14_max) << "\t" << range2_14 << "\n";
    std::cout << "FP4_20\t\t" << static_cast<double>(fp4_20_min) << "\t" << static_cast<double>(fp4_20_max) << "\t" << range4_20 << "\n";
    std::cout << "FP8_24\t\t" << static_cast<double>(fp8_24_min) << "\t" << static_cast<double>(fp8_24_max) << "\t" << range8_24 << "\n";

    // Pip hassasiyeti test et
    std::cout << "\n=== Pip Resolution Test ===\n";
    std::cout << "Configuration\tPip Resolution\tPips per LSB\n";

    double resolution2_14 = 1.0 / (1 << 14);
    double resolution4_20 = 1.0 / (1 << 20);
    double resolution8_24 = 1.0 / (1 << 24);

    std::cout << "FP2_14\t\t" << resolution2_14 << "\t" << pip / resolution2_14 << "\n";
    std::cout << "FP4_20\t\t" << resolution4_20 << "\t" << pip / resolution4_20 << "\n";
    std::cout << "FP8_24\t\t" << resolution8_24 << "\t" << pip / resolution8_24 << "\n";
}

} // namespace gel::test