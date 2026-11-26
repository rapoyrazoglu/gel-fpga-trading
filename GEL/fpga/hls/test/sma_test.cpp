
// fpga/hls/test/sma_test.cpp
#include "sma_hls.hpp"
#include <iostream>
#include <vector>
#include <iomanip>

// HLS simülasyonu için basit test fonksiyonu
int main() {
    const int SIZE = 10;
    const int WINDOW = 3;

    // Test verileri
    gel::fpga::FixedPoint<10, 6> input_data[SIZE];
    gel::fpga::FixedPoint<10, 6> output_data[SIZE];

    // Veri hazırlama
    for (int i = 0; i < SIZE; ++i) {
        input_data[i] = gel::fpga::FixedPoint<10, 6>(1.0 + 0.1 * i);
    }

    // SMA hesaplama
    gel::fpga::SMA_HLS<WINDOW> sma;

    // Her değeri işle ve sonuçları yazdır
    std::cout << "Input\tSMA-3\n";
    std::cout << "-----\t-----\n";

    for (int i = 0; i < SIZE; ++i) {
        output_data[i] = sma.update(input_data[i]);
        float in_val = static_cast<float>(input_data[i]);
        float out_val = static_cast<float>(output_data[i]);

        std::cout << std::fixed << std::setprecision(4);
        std::cout << in_val << "\t" << out_val << "\n";
    }

    return 0;
}