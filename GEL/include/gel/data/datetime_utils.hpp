// include/gel/data/datetime_utils.hpp
#pragma once

#include <chrono>
#include <string>
#include <regex>
#include <iomanip>
#include <sstream>

namespace gel::data {

    class DateTimeUtils {
    public:
        // ISO 8601 formatındaki string'i time_point'e dönüştür
        static std::chrono::system_clock::time_point parseISO8601(const std::string& iso8601_str) {
            // ISO 8601 formatı: 2003-05-05 00:00:05.536000+00:00

            std::tm tm = {};
            std::istringstream ss(iso8601_str);

            ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");

            // Mikrosaniye parse et
            double fractional_seconds = 0.0;
            if (ss.peek() == '.') {
                ss.ignore();
                int microseconds;
                ss >> microseconds;
                fractional_seconds = microseconds / 1000000.0;
            }

            auto tp = std::chrono::system_clock::from_time_t(std::mktime(&tm));

            // Mikrosaniye ekle
            tp += std::chrono::microseconds(static_cast<long>(fractional_seconds * 1000000));

            return tp;
        }

        // Time point'i ISO 8601 formatına dönüştür
        static std::string toISO8601(const std::chrono::system_clock::time_point& tp) {
            auto time_t = std::chrono::system_clock::to_time_t(tp);
            auto ms = std::chrono::duration_cast<std::chrono::microseconds>(
                tp.time_since_epoch() % std::chrono::seconds(1));

            std::stringstream ss;
            // Thread-safe gmtime kullan (UTC için zaten gerekli)
            ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d %H:%M:%S");
            ss << '.' << std::setfill('0') << std::setw(6) << ms.count();
            ss << "+00:00";  // UTC

            return ss.str();
        }
    };

} // namespace gel::data