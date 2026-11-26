// include/gel/data/dukascopy_parser.hpp
#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <functional>
#include "gel/data/tick.hpp"
#include "gel/data/datetime_utils.hpp"

namespace gel::data {

class DukascopyParser {
public:
    // Dukascopy CSV dosyasını parse et ve Tick vektörü döndür
    static std::vector<Tick> parseTickFile(const std::string& filename) {
        std::vector<Tick> ticks;
        std::ifstream file(filename);

        if (!file.is_open()) {
            throw std::runtime_error("Could not open file: " + filename);
        }

        std::string line;
        // Header satırını atla
        std::getline(file, line);

        // Satırları parse et
        while (std::getline(file, line)) {
            std::istringstream ss(line);
            std::string field;
            Tick tick;

            // Datetime
            if (std::getline(ss, field, ',')) {
                tick.timestamp = DateTimeUtils::parseISO8601(field);
            } else {
                continue; // Geçersiz satır, atla
            }

            // Bid
            if (std::getline(ss, field, ',')) {
                tick.bid = std::stod(field);
            } else {
                continue;
            }

            // Ask
            if (std::getline(ss, field, ',')) {
                tick.ask = std::stod(field);
            } else {
                continue;
            }

            // Bid Volume
            if (std::getline(ss, field, ',')) {
                tick.bid_volume = std::stod(field);
            } else {
                continue;
            }

            // Ask Volume
            if (std::getline(ss, field)) {
                tick.ask_volume = std::stod(field);
            } else {
                continue;
            }

            ticks.push_back(tick);
        }

        return ticks;
    }

    // Daha hızlı işlem için batch processing versiyonu (opsiyonel)
    static void parseTicksInBatches(
            const std::string& filename,
            size_t batch_size,
            const std::function<void(const std::vector<Tick>&)>& processor) {

        std::ifstream file(filename);

        if (!file.is_open()) {
            throw std::runtime_error("Could not open file: " + filename);
        }

        std::string line;
        // Header satırını atla
        std::getline(file, line);

        std::vector<Tick> batch;
        batch.reserve(batch_size);

        // Satırları batch olarak işle
        while (std::getline(file, line)) {
            std::istringstream ss(line);
            std::string field;
            Tick tick;

            // Datetime
            if (std::getline(ss, field, ',')) {
                tick.timestamp = DateTimeUtils::parseISO8601(field);
            } else {
                continue; // Geçersiz satır, atla
            }

            // Bid
            if (std::getline(ss, field, ',')) {
                tick.bid = std::stod(field);
            } else {
                continue;
            }

            // Ask
            if (std::getline(ss, field, ',')) {
                tick.ask = std::stod(field);
            } else {
                continue;
            }

            // Bid Volume
            if (std::getline(ss, field, ',')) {
                tick.bid_volume = std::stod(field);
            } else {
                continue;
            }

            // Ask Volume
            if (std::getline(ss, field)) {
                tick.ask_volume = std::stod(field);
            } else {
                continue;
            }

            batch.push_back(tick);

            if (batch.size() >= batch_size) {
                processor(batch);
                batch.clear();
                batch.reserve(batch_size);
            }
        }

        // Son batch'i işle
        if (!batch.empty()) {
            processor(batch);
        }
    }
};

} // namespace gel::data