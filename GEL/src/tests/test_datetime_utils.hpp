// src/tests/test_datetime_utils.hpp
#pragma once

#include <iostream>
#include <vector>
#include "gel/data/datetime_utils.hpp"

namespace gel::test {

    void test_datetime_utils() {
        std::cout << "Testing DateTime Utils\n";

        // ISO 8601 format test
        std::vector<std::string> test_datetimes = {
            "2003-05-05 00:00:05.536000+00:00",
            "2025-10-14 19:59:59.689000+00:00",
            "2023-12-31 23:59:59.999000+00:00"
        };

        for (const auto& dt_str : test_datetimes) {
            // String -> time_point
            auto tp = gel::data::DateTimeUtils::parseISO8601(dt_str);

            // time_point -> String
            auto converted_str = gel::data::DateTimeUtils::toISO8601(tp);

            std::cout << "Original: " << dt_str << "\n";
            std::cout << "Converted: " << converted_str << "\n";
            std::cout << "Match: " << (dt_str == converted_str ? "Yes" : "No") << "\n\n";
        }
    }

} // namespace gel::test