// src/tests/test_utils.hpp
#pragma once

#include <iostream>
#include <string>
#include <functional>
#include <chrono>
#include <filesystem>
#include <cmath>
#include <stdexcept>
#include "gel/testing/test_reporter.hpp"
#include "gel/testing/logger.hpp"

namespace gel::test {

// Assertion Macros for Pattern Tests
#define ASSERT_TRUE(condition) \
    if (!(condition)) { \
        throw std::runtime_error("ASSERT_TRUE failed: " #condition); \
    }

#define ASSERT_FALSE(condition) \
    if (condition) { \
        throw std::runtime_error("ASSERT_FALSE failed: " #condition); \
    }

#define ASSERT_EQUAL(a, b) \
    if ((a) != (b)) { \
        throw std::runtime_error("ASSERT_EQUAL failed: " #a " != " #b); \
    }

#define ASSERT_EQUAL_APPROX(a, b, epsilon) \
    if (std::abs((a) - (b)) > (epsilon)) { \
        throw std::runtime_error("ASSERT_EQUAL_APPROX failed: " #a " ≈ " #b); \
    }

    // Test çalıştırma fonksiyonu - reporting ve logging ile
    inline void run_test(const std::string& test_name, const std::function<void()>& test_func) {
        std::cout << "\n====================================\n";
        std::cout << "RUNNING TEST: " << test_name << "\n";
        std::cout << "====================================\n";

        gel::testing::Logger::getInstance().info("Starting test: " + test_name);

        gel::testing::TestResult result;
        result.test_name = test_name;
        result.timestamp = std::chrono::system_clock::now();

        auto start = std::chrono::high_resolution_clock::now();

        try {
            test_func();

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

            result.passed = true;
            result.duration_ms = duration.count();
            result.error_message = "";

            std::cout << "\n✓ TEST PASSED: " << test_name << " (in " << duration.count() << " ms)\n";
            gel::testing::Logger::getInstance().info("Test PASSED: " + test_name +
                                                      " (duration: " + std::to_string(duration.count()) + " ms)");
        } catch (const std::exception& e) {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

            result.passed = false;
            result.duration_ms = duration.count();
            result.error_message = e.what();

            std::cerr << "\n✗ TEST FAILED: " << test_name << "\n";
            std::cerr << "Error: " << e.what() << std::endl;

            gel::testing::Logger::getInstance().error("Test FAILED: " + test_name);
            gel::testing::Logger::getInstance().error("Error message: " + std::string(e.what()));
        }

        gel::testing::TestReporter::addResult(result);
    }

    // Test raporu oluşturma helper fonksiyonları
    inline void export_test_reports(const std::string& output_dir = "test_results") {
        // Dizini oluştur
        std::filesystem::create_directories(output_dir);

        // Raporları export et
        gel::testing::TestReporter::exportJSON(output_dir + "/test_results.json");
        gel::testing::TestReporter::exportCSV(output_dir + "/test_results.csv");
        gel::testing::TestReporter::exportMarkdown(output_dir + "/test_results.md");

        std::cout << "\nTest reports exported to: " << output_dir << "/\n";
        std::cout << "  - test_results.json\n";
        std::cout << "  - test_results.csv\n";
        std::cout << "  - test_results.md\n";
    }

} // namespace gel::test
