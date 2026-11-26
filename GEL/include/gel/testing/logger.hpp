// include/gel/testing/logger.hpp
#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <mutex>

namespace gel::testing {

enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    CRITICAL
};

class Logger {
public:
    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    // Delete copy constructor and assignment
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void init(const std::string& log_file = "gel_tests.log", LogLevel min_level = LogLevel::INFO) {
        std::lock_guard<std::mutex> lock(mutex_);

        min_level_ = min_level;
        log_file_path_ = log_file;

        // Ensure log directory exists
        std::filesystem::path log_path(log_file);
        if (log_path.has_parent_path()) {
            std::filesystem::create_directories(log_path.parent_path());
        }

        // Open log file in append mode
        log_file_.open(log_file, std::ios::app);
        if (!log_file_.is_open()) {
            std::cerr << "Warning: Could not open log file: " << log_file << std::endl;
        }

        // Write session header (using unlocked version to avoid deadlock)
        logUnlocked("=================================================", LogLevel::INFO);
        logUnlocked("GEL Trading System - Test Session Started", LogLevel::INFO);
        logUnlocked("=================================================", LogLevel::INFO);
    }

    void log(const std::string& message, LogLevel level = LogLevel::INFO) {
        if (level < min_level_) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        std::string timestamp = getCurrentTimestamp();
        std::string level_str = levelToString(level);

        std::stringstream log_entry;
        log_entry << "[" << timestamp << "] [" << level_str << "] " << message;

        // Console output
        if (level >= LogLevel::ERROR) {
            std::cerr << log_entry.str() << std::endl;
        } else {
            std::cout << log_entry.str() << std::endl;
        }

        // File output
        if (log_file_.is_open()) {
            log_file_ << log_entry.str() << std::endl;
            log_file_.flush();  // Ensure immediate write
        }
    }

    void debug(const std::string& message) {
        log(message, LogLevel::DEBUG);
    }

    void info(const std::string& message) {
        log(message, LogLevel::INFO);
    }

    void warning(const std::string& message) {
        log(message, LogLevel::WARNING);
    }

    void error(const std::string& message) {
        log(message, LogLevel::ERROR);
    }

    void critical(const std::string& message) {
        log(message, LogLevel::CRITICAL);
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (log_file_.is_open()) {
            logUnlocked("=================================================", LogLevel::INFO);
            logUnlocked("GEL Trading System - Test Session Ended", LogLevel::INFO);
            logUnlocked("=================================================", LogLevel::INFO);
            log_file_.close();
        }
    }

    void setMinLevel(LogLevel level) {
        min_level_ = level;
    }

    std::string getLogFilePath() const {
        return log_file_path_;
    }

private:
    Logger() : min_level_(LogLevel::INFO) {}

    ~Logger() {
        close();
    }

    // Internal logging method without mutex lock (for use when mutex is already locked)
    void logUnlocked(const std::string& message, LogLevel level = LogLevel::INFO) {
        if (level < min_level_) {
            return;
        }

        std::string timestamp = getCurrentTimestamp();
        std::string level_str = levelToString(level);

        std::stringstream log_entry;
        log_entry << "[" << timestamp << "] [" << level_str << "] " << message;

        // Console output
        if (level >= LogLevel::ERROR) {
            std::cerr << log_entry.str() << std::endl;
        } else {
            std::cout << log_entry.str() << std::endl;
        }

        // File output
        if (log_file_.is_open()) {
            log_file_ << log_entry.str() << std::endl;
            log_file_.flush();  // Ensure immediate write
        }
    }

    std::string getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);

        // Get milliseconds
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ) % 1000;

        std::stringstream ss;
        ss << std::put_time(std::gmtime(&time_t_now), "%Y-%m-%d %H:%M:%S")
           << "." << std::setfill('0') << std::setw(3) << ms.count();

        return ss.str();
    }

    std::string levelToString(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG:    return "DEBUG   ";
            case LogLevel::INFO:     return "INFO    ";
            case LogLevel::WARNING:  return "WARNING ";
            case LogLevel::ERROR:    return "ERROR   ";
            case LogLevel::CRITICAL: return "CRITICAL";
            default:                 return "UNKNOWN ";
        }
    }

    std::ofstream log_file_;
    std::string log_file_path_;
    LogLevel min_level_;
    std::mutex mutex_;
};

// Helper macros for easier logging
#define LOG_DEBUG(msg) gel::testing::Logger::getInstance().debug(msg)
#define LOG_INFO(msg) gel::testing::Logger::getInstance().info(msg)
#define LOG_WARNING(msg) gel::testing::Logger::getInstance().warning(msg)
#define LOG_ERROR(msg) gel::testing::Logger::getInstance().error(msg)
#define LOG_CRITICAL(msg) gel::testing::Logger::getInstance().critical(msg)

} // namespace gel::testing
