// src/tests/test_csv_reader.hpp
#pragma once

#include <iostream>
#include <iomanip>
#include <chrono>
#include "gel/data/csv_reader.hpp"

namespace gel::test {

void test_csv_reader() {
    std::cout << "Testing CSV Reader\n";

    // Dukascopy veri dosyası
    const std::string filename = "data/EURUSD_all_ticks_20250601_20251014.csv";
    // Daha küçük test dosyası için: const std::string filename = "data/EURUSD_sample.csv";

    // Dosya istatistiklerini yazdır
    size_t rowCount = gel::data::CSVReader::getRowCount(filename);
    std::cout << "=== CSV Dosya Bilgileri ===\n";
    std::cout << "Dosya: " << filename << "\n";
    std::cout << "Toplam satir sayisi: " << rowCount << "\n";

    // Header'ı oku ve yazdır
    auto header = gel::data::CSVReader::getHeader(filename);

    std::cout << "\n=== CSV Header ===\n";
    for (size_t i = 0; i < header.size(); ++i) {
        std::cout << i << ": \"" << header[i] << "\"\n";
    }

    // Performans ölçümü için kronometre
    auto start = std::chrono::high_resolution_clock::now();

    // CSV'yi parse et - küçük dosya kullanılmalı veya parse'ın limitleri
    std::cout << "\n=== CSV Parsing ===\n";
    std::cout << "Reading first 1000 rows for testing...\n";
    
    auto data = gel::data::CSVReader::parseCSV(filename, ',', true, 1000);

    // Süreyi ölç
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "CSV parse süresi: " << duration.count() << " ms\n";
    std::cout << "Okunan satır sayısı: " << data.size() << "\n";

    // İlk 5 satırı yazdır
    std::cout << "\n=== Ilk 5 Satir ===\n";
    for (size_t i = 0; i < std::min(data.size(), size_t(5)); ++i) {
        std::cout << "Satır " << i << ": ";
        for (const auto& field : data[i]) {
            std::cout << std::setw(15) << field;
        }
        std::cout << "\n";
    }

    // Son 5 satırı yazdır
    std::cout << "\n=== Son 5 Satir ===\n";
    for (size_t i = data.size() - std::min(data.size(), size_t(5)); i < data.size(); ++i) {
        std::cout << "Satir " << i << ": ";
        for (const auto& field : data[i]) {
            std::cout << std::setw(15) << field;
        }
        std::cout << "\n";
    }

    // Her sütun için veri tipi analizi
    if (!data.empty() && !header.empty()) {
        std::cout << "\n=== Sutun Veri Tipi Analizi ===\n";

        for (size_t col = 0; col < header.size(); col++) {
            bool isInteger = true;
            bool isDouble = true;
            bool isEmpty = true;

            for (size_t row = 0; row < std::min(data.size(), size_t(100)); row++) {
                if (col < data[row].size()) {
                    const std::string& value = data[row][col];

                    if (!value.empty()) {
                        isEmpty = false;

                        // Integer kontrolü
                        try {
                            std::stoll(value);
                        } catch (...) {
                            isInteger = false;
                        }

                        // Double kontrolü
                        if (!isInteger) {
                            try {
                                std::stod(value);
                            } catch (...) {
                                isDouble = false;
                            }
                        }
                    }
                }
            }

            std::cout << "Sutun " << col << " \"" << header[col] << "\": ";
            if (isEmpty) {
                std::cout << "Bos\n";
            } else if (isInteger) {
                std::cout << "Integer\n";
            } else if (isDouble) {
                std::cout << "Double\n";
            } else {
                std::cout << "String\n";
            }
        }
    }
}

} // namespace gel::test