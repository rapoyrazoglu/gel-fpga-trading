#include "gel/common.hpp"
#include <iostream>
#include <vector>
#include <ranges>

int main() {
    gel::Logger::info("GEL Trading System v{} - Initialization", gel::VERSION);

    std::vector<double> prices = {1.1234, 1.1235, 1.1233, 1.1236, 1.1237, 1.1232};


    // std::views::slide is C++23 and only ships in libc++ trunk; the project
    // is built against AppleClang/libc++ which doesn't have it yet. Manual
    // sliding-window over indices is portable and equivalent.
    constexpr std::size_t WINDOW = 3;
    auto moving_avg = std::views::iota(std::size_t{0}, prices.size() - WINDOW + 1)
        | std::views::transform([&prices](std::size_t i) {
            double sum = 0.0;
            for (std::size_t k = 0; k < WINDOW; ++k) sum += prices[i + k];
            return sum / static_cast<double>(WINDOW);
        });



    gel::Logger::info("Example Moving Average Calculation:");
    for (auto avg : moving_avg) {
        std::cout << std::format("  MA3: {:.6f}\n", avg);
    }




    auto start = std::chrono::high_resolution_clock::now();
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    gel::Logger::info("Calculation took {} µs", duration.count());


    gel::json system_info = {
        {"name", "GEL Trading System"},
        {"version", gel::VERSION},
        {"currency_pair", "EUR/USD"},
        {"cpp_standard", "C++23/26"},
        {"features", {"ranges", "concepts", "format", "modules"}}
    };

    std::cout << std::format("System Info: {}\n", system_info.dump(2));



    return 0;



}