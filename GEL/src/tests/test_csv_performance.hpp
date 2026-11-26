// src/tests/test_csv_performance.hpp
#pragma once

#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <chrono>
#include <vector>
#include <string>
#include "gel/data/csv_reader.hpp"

namespace gel::test {

void test_csv_performance() {
    std::cout << "Testing CSV Performance\n";
    
    // Ana dosya yolu
    const std::string large_filename = "data/EURUSD_all_ticks_20250601_20251014.csv";
    // Daha küçük test dosyası (varsa)
    const std::string sample_filename = "data/EURUSD_sample.csv";
    
    // Kullanılacak dosyayı belirle (küçük dosya varsa onu kullan)
    std::ifstream check_sample(sample_filename);
    const std::string& filename = check_sample.good() ? sample_filename : large_filename;
    check_sample.close();
    
    std::cout << "=== CSV Okuma Performans Testi ===\n";
    std::cout << "Dosya: " << filename << "\n";

    // Doğrudan ifstream ile okuma
    {
        auto start = std::chrono::high_resolution_clock::now();

        std::ifstream file(filename);
        std::string line;
        int lineCount = 0;

        while (std::getline(file, line)) {
            lineCount++;
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "Ifstream ile satirlari okuma: " << duration.count() << " ms (" << lineCount << " satir)\n";
    }

    // readLines metodu ile okuma
    {
        auto start = std::chrono::high_resolution_clock::now();

        auto lines = gel::data::CSVReader::readLines(filename);

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "readLines ile okuma: " << duration.count() << " ms (" << lines.size() << " satir)\n";
    }

    // parseCSV metodu ile okuma
    {
        auto start = std::chrono::high_resolution_clock::now();

        // Büyük dosya için satır sayısını sınırla
        const size_t MAX_ROWS = 100000;  // 100K satır ile sınırla
        auto data = filename == large_filename 
                    ? gel::data::CSVReader::parseCSV(filename, ',', true, MAX_ROWS)
                    : gel::data::CSVReader::parseCSV(filename);

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "parseCSV ile okuma ve parse etme: " << duration.count() << " ms (" << data.size() << " satir)\n";
    }
    
    // CSV okuma bellek kullanımını test et
    std::cout << "\n=== CSV Bellek Kullanimi Testi ===\n";

    // Dosya boyutunu al
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (file.is_open()) {
        std::streamsize fileSize = file.tellg();
        std::cout << "Dosya boyutu: " << fileSize / 1024.0 / 1024.0 << " MB\n";
        file.close();
    }

    // Satır sayısını al
    size_t lineCount = gel::data::CSVReader::getRowCount(filename);
    std::cout << "Satir sayisi: " << lineCount << "\n";

    // İlk 10000 satır için bellek kullanımını tahmin et
    const size_t SAMPLE_SIZE = std::min(lineCount, size_t(10000));

    auto data = gel::data::CSVReader::parseCSV(filename, ',', true, SAMPLE_SIZE);

    // Bellek kullanımını analiz et
    size_t totalChars = 0;
    size_t rowCount = 0;

    for (const auto& row : data) {
        if (rowCount >= SAMPLE_SIZE) break;

        for (const auto& field : row) {
            totalChars += field.size();
        }
        rowCount++;
    }

    double avgCharsPerRow = static_cast<double>(totalChars) / rowCount;
    double estimatedTotalChars = avgCharsPerRow * lineCount;

    std::cout << "Ortalama karakter/satir: " << avgCharsPerRow << "\n";
    std::cout << "Tahmini toplam bellek kullaimi: " << estimatedTotalChars / 1024.0 / 1024.0 << " MB\n";
}

} // namespace gel::test