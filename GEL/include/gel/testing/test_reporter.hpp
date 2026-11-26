// include/gel/testing/test_reporter.hpp
#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <mutex>
#include <iostream>

namespace gel::testing {

struct TestResult {
    std::string test_name;
    bool passed;
    double duration_ms;
    std::string error_message;
    std::chrono::system_clock::time_point timestamp;
};

class TestReporter {
public:
    static void addResult(const TestResult& result) {
        std::lock_guard<std::mutex> lock(mutex_);
        results_.push_back(result);
    }

    // Export to JSON
    static void exportJSON(const std::string& filepath) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ofstream file(filepath);
        if (!file.is_open()) {
            throw std::runtime_error("Could not open file: " + filepath);
        }

        file << "{\n";
        file << "  \"test_results\": [\n";

        for (size_t i = 0; i < results_.size(); ++i) {
            const auto& result = results_[i];

            file << "    {\n";
            file << "      \"test_name\": \"" << result.test_name << "\",\n";
            file << "      \"passed\": " << (result.passed ? "true" : "false") << ",\n";
            file << "      \"duration_ms\": " << result.duration_ms << ",\n";
            file << "      \"error_message\": \"" << escapeJSON(result.error_message) << "\",\n";
            file << "      \"timestamp\": \"" << formatTimestamp(result.timestamp) << "\"\n";
            file << "    }";

            if (i < results_.size() - 1) {
                file << ",";
            }
            file << "\n";
        }

        file << "  ],\n";
        file << "  \"summary\": {\n";
        file << "    \"total\": " << results_.size() << ",\n";
        file << "    \"passed\": " << countPassedUnlocked() << ",\n";
        file << "    \"failed\": " << countFailedUnlocked() << ",\n";
        file << "    \"total_duration_ms\": " << totalDurationUnlocked() << "\n";
        file << "  }\n";
        file << "}\n";

        file.close();
    }

    // Export to CSV
    static void exportCSV(const std::string& filepath) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ofstream file(filepath);
        if (!file.is_open()) {
            throw std::runtime_error("Could not open file: " + filepath);
        }

        // Header
        file << "Test Name,Passed,Duration (ms),Error Message,Timestamp\n";

        // Results
        for (const auto& result : results_) {
            file << escapeCSV(result.test_name) << ","
                 << (result.passed ? "PASS" : "FAIL") << ","
                 << result.duration_ms << ","
                 << escapeCSV(result.error_message) << ","
                 << formatTimestamp(result.timestamp) << "\n";
        }

        file.close();
    }

    // Export to Markdown
    static void exportMarkdown(const std::string& filepath) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ofstream file(filepath);
        if (!file.is_open()) {
            throw std::runtime_error("Could not open file: " + filepath);
        }

        file << "# GEL Trading System - Test Report\n\n";
        file << "**Date:** " << formatTimestamp(std::chrono::system_clock::now()) << "\n\n";

        // Summary
        file << "## Summary\n\n";
        file << "| Metric | Value |\n";
        file << "|--------|-------|\n";
        file << "| Total Tests | " << results_.size() << " |\n";
        file << "| Passed | " << countPassedUnlocked() << " |\n";
        file << "| Failed | " << countFailedUnlocked() << " |\n";
        file << "| Total Duration | " << totalDurationUnlocked() << " ms |\n\n";

        // Detailed results
        file << "## Test Results\n\n";
        file << "| Test Name | Status | Duration (ms) | Error Message |\n";
        file << "|-----------|--------|---------------|---------------|\n";

        for (const auto& result : results_) {
            file << "| " << result.test_name << " | ";
            file << (result.passed ? "✅ PASS" : "❌ FAIL") << " | ";
            file << std::fixed << std::setprecision(2) << result.duration_ms << " | ";
            file << (result.error_message.empty() ? "-" : result.error_message) << " |\n";
        }

        file.close();
    }

    // Print summary to console
    static void printSummary() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << "\n========================================\n";
        std::cout << "        TEST EXECUTION SUMMARY\n";
        std::cout << "========================================\n";
        std::cout << "Total Tests:     " << results_.size() << "\n";
        auto passed = countPassedUnlocked();
        auto failed = results_.size() - passed;
        std::cout << "Passed:          " << passed << " ("
                  << (results_.empty() ? 0 : (passed * 100.0 / results_.size())) << "%)\n";
        std::cout << "Failed:          " << failed << " ("
                  << (results_.empty() ? 0 : (failed * 100.0 / results_.size())) << "%)\n";
        std::cout << "Total Duration:  " << totalDurationUnlocked() << " ms\n";
        std::cout << "========================================\n\n";

        if (failed > 0) {
            std::cout << "Failed Tests:\n";
            for (const auto& result : results_) {
                if (!result.passed) {
                    std::cout << "  ❌ " << result.test_name << ": "
                              << result.error_message << "\n";
                }
            }
        }
    }

    static void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        results_.clear();
    }

    static size_t countPassed() {
        std::lock_guard<std::mutex> lock(mutex_);
        return countPassedUnlocked();
    }

    static size_t countFailed() {
        std::lock_guard<std::mutex> lock(mutex_);
        return countFailedUnlocked();
    }

    static double totalDuration() {
        std::lock_guard<std::mutex> lock(mutex_);
        return totalDurationUnlocked();
    }

private:
    static std::vector<TestResult> results_;
    static std::mutex mutex_;

    // Internal helper methods (called when mutex is already locked)
    static size_t countPassedUnlocked() {
        size_t count = 0;
        for (const auto& result : results_) {
            if (result.passed) count++;
        }
        return count;
    }

    static double totalDurationUnlocked() {
        double total = 0.0;
        for (const auto& result : results_) {
            total += result.duration_ms;
        }
        return total;
    }

    static size_t countFailedUnlocked() {
        return results_.size() - countPassedUnlocked();
    }

    static std::string escapeJSON(const std::string& str) {
        std::string result;
        for (char c : str) {
            switch (c) {
                case '"':  result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\b': result += "\\b"; break;
                case '\f': result += "\\f"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default:   result += c; break;
            }
        }
        return result;
    }

    static std::string escapeCSV(const std::string& str) {
        if (str.find(',') != std::string::npos ||
            str.find('"') != std::string::npos ||
            str.find('\n') != std::string::npos) {
            std::string result = "\"";
            for (char c : str) {
                if (c == '"') result += "\"\"";
                else result += c;
            }
            result += "\"";
            return result;
        }
        return str;
    }

    static std::string formatTimestamp(const std::chrono::system_clock::time_point& tp) {
        auto time_t_val = std::chrono::system_clock::to_time_t(tp);
        std::stringstream ss;
        ss << std::put_time(std::gmtime(&time_t_val), "%Y-%m-%d %H:%M:%S UTC");
        return ss.str();
    }
};

// Static member definitions
inline std::vector<TestResult> TestReporter::results_;
inline std::mutex TestReporter::mutex_;

} // namespace gel::testing
