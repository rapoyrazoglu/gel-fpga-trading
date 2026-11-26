#pragma once
#include <iostream>
#include <string_view>
#include <chrono>
#include <concepts>
#include <ranges>
#include <format>
#include <source_location>
#include <nlohmann/json.hpp>

namespace gel {
    using json = nlohmann::json;
    
    // C++20 string literals
    using namespace std::string_view_literals;
    
    // C++20 chrono literals
    using namespace std::chrono_literals;
    
    // Modern C++ zaman tipleri
    using TimePoint = std::chrono::sys_time<std::chrono::microseconds>;
    
    // Versiyon bilgisi
    constexpr auto VERSION = "0.2.0"sv;
    
    // C++20 concepts - Sayısal tip kontrolü
    template <typename T>
    concept Numeric = std::integral<T> || std::floating_point<T>;
    
    // C++23 format logger (daha gelişmiş olanı 3. haftada yapacağız)
    class Logger {
    public:
        template <typename... Args>
        static void info(std::format_string<Args...> fmt, Args&&... args) {
            auto now = std::chrono::system_clock::now();
            auto message = std::format(fmt, std::forward<Args>(args)...);
            std::cout << std::format("[{}] INFO: {}\n", 
                       std::chrono::floor<std::chrono::milliseconds>(now).time_since_epoch().count(),
                       message);
        }
    };
}