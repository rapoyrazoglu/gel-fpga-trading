// src/tests/test_data_analyzer.hpp
#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <limits>
#include <chrono>
#include <iomanip>
#include <cmath>

namespace gel::test {

void test_data_analyzer() {
    std::cout << "Testing Data Analyzer\n";
    
    // Dukascopy veri dosyası
    const std::string filename = "data/EURUSD_all_ticks_20250601_20251014.csv";
    // Daha küçük test için: const std::string filename = "data/EURUSD_sample.csv";
    
    struct TickData {
        uint64_t timestamp;
        double bid;
        double ask;
        double volume;
    };
    
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Dosya açılamadı: " << filename << std::endl;
        throw std::runtime_error("Could not open file");
    }
    
    // İlk 10 satırı incele
    std::string line;
    int lineCount = 0;
    
    std::cout << "=== Dukascopy Veri Örnekleri ===\n";
    std::cout << "İlk 10 satır:\n";
    
    // İlk satır genellikle başlıktır
    if (std::getline(file, line)) {
        std::cout << "Header: " << line << std::endl;
    }
    
    while (std::getline(file, line) && lineCount < 10) {
        std::cout << lineCount << ": " << line << std::endl;
        lineCount++;
    }
    
    // Dosyayı başa sar ve header satırını atla
    file.clear();
    file.seekg(0, std::ios::beg);
    std::getline(file, line); // Header satırını atla
    
    // Verileri oku ve istatistikleri hesapla
    std::vector<TickData> ticks;
    
    // Büyük dosyalar için sadece ilk 10,000 satırı analiz et
    int maxRows = 10000;
    int rowCount = 0;
    
    while (std::getline(file, line) && rowCount < maxRows) {
        std::istringstream ss(line);
        std::string token;
        TickData tick;
        
        // Timestamp
        if (std::getline(ss, token, ',')) {
            try {
                tick.timestamp = std::stoull(token);
            } catch (...) {
                // Hatalı format, atla
                continue;
            }
        } else {
            continue;
        }
        
        // Bid
        if (std::getline(ss, token, ',')) {
            try {
                tick.bid = std::stod(token);
            } catch (...) {
                continue;
            }
        } else {
            continue;
        }
        
        // Ask
        if (std::getline(ss, token, ',')) {
            try {
                tick.ask = std::stod(token);
            } catch (...) {
                continue;
            }
        } else {
            continue;
        }
        
        // Volume (opsiyonel olabilir)
        if (std::getline(ss, token, ',')) {
            try {
                tick.volume = std::stod(token);
            } catch (...) {
                tick.volume = 0.0;
            }
        } else {
            tick.volume = 0.0;
        }
        
        ticks.push_back(tick);
        rowCount++;
    }
    
    // Veri istatistiklerini hesapla
    std::cout << "\n=== Veri İstatistikleri ===\n";
    std::cout << "Toplam tick sayısı: " << ticks.size() << std::endl;
    
    if (ticks.empty()) {
        std::cout << "Veri yok, istatistik hesaplanamıyor." << std::endl;
        return;
    }
    
    // Min/Max bid ve ask değerleri
    double minBid = std::numeric_limits<double>::max();
    double maxBid = std::numeric_limits<double>::lowest();
    double sumBid = 0.0;
    
    double minAsk = std::numeric_limits<double>::max();
    double maxAsk = std::numeric_limits<double>::lowest();
    double sumAsk = 0.0;
    
    double minSpread = std::numeric_limits<double>::max();
    double maxSpread = 0.0;
    double sumSpread = 0.0;
    
    double totalVolume = 0.0;
    
    uint64_t minTimestamp = std::numeric_limits<uint64_t>::max();
    uint64_t maxTimestamp = 0;
    
    // Tick aralıkları için
    std::vector<uint64_t> timeDiffs;
    
    for (size_t i = 0; i < ticks.size(); i++) {
        const auto& tick = ticks[i];
        
        // Bid istatistikleri
        minBid = std::min(minBid, tick.bid);
        maxBid = std::max(maxBid, tick.bid);
        sumBid += tick.bid;
        
        // Ask istatistikleri
        minAsk = std::min(minAsk, tick.ask);
        maxAsk = std::max(maxAsk, tick.ask);
        sumAsk += tick.ask;
        
        // Spread istatistikleri
        double spread = tick.ask - tick.bid;
        minSpread = std::min(minSpread, spread);
        maxSpread = std::max(maxSpread, spread);
        sumSpread += spread;
        
        // Volume istatistikleri
        totalVolume += tick.volume;
        
        // Timestamp istatistikleri
        minTimestamp = std::min(minTimestamp, tick.timestamp);
        maxTimestamp = std::max(maxTimestamp, tick.timestamp);
        
        // Tick aralıkları
        if (i > 0) {
            uint64_t diff = tick.timestamp - ticks[i-1].timestamp;
            timeDiffs.push_back(diff);
        }
    }
    
    double avgBid = sumBid / ticks.size();
    double avgAsk = sumAsk / ticks.size();
    double avgSpread = sumSpread / ticks.size();
    
    // Zaman aralıkları için istatistikler
    if (!timeDiffs.empty()) {
        std::sort(timeDiffs.begin(), timeDiffs.end());
        
        uint64_t minDiff = timeDiffs.front();
        uint64_t maxDiff = timeDiffs.back();
        
        // Ortalama ve medyan hesapla
        uint64_t sumDiff = 0;
        for (auto diff : timeDiffs) {
            sumDiff += diff;
        }
        double avgDiff = static_cast<double>(sumDiff) / timeDiffs.size();
        
        uint64_t medianDiff;
        if (timeDiffs.size() % 2 == 0) {
            medianDiff = (timeDiffs[timeDiffs.size()/2 - 1] + timeDiffs[timeDiffs.size()/2]) / 2;
        } else {
            medianDiff = timeDiffs[timeDiffs.size()/2];
        }
        
        // İlk ve son timestamp'leri tarih/saat formatına dönüştür
        std::time_t firstTime = minTimestamp / 1000000; // Mikrosaniyeden saniyeye çevir
        std::time_t lastTime = maxTimestamp / 1000000;  // Mikrosaniyeden saniyeye çevir
        
        std::cout << "\n=== Zaman Aralığı ===\n";
        std::cout << "İlk tick: " << std::put_time(std::gmtime(&firstTime), "%Y-%m-%d %H:%M:%S UTC") << std::endl;
        std::cout << "Son tick: " << std::put_time(std::gmtime(&lastTime), "%Y-%m-%d %H:%M:%S UTC") << std::endl;
        std::cout << "Toplam süre: " << (maxTimestamp - minTimestamp) / 1000000.0 / 60.0 / 60.0 / 24.0 << " gün" << std::endl;
        
        std::cout << "\n=== Tick Aralıkları (mikrosaniye) ===\n";
        std::cout << "Minimum aralık: " << minDiff << " mikrosaniye (" << minDiff / 1000.0 << " ms)" << std::endl;
        std::cout << "Maksimum aralık: " << maxDiff << " mikrosaniye (" << maxDiff / 1000000.0 << " saniye)" << std::endl;
        std::cout << "Ortalama aralık: " << avgDiff << " mikrosaniye (" << avgDiff / 1000.0 << " ms)" << std::endl;
        std::cout << "Medyan aralık: " << medianDiff << " mikrosaniye (" << medianDiff / 1000.0 << " ms)" << std::endl;
    }
    
    std::cout << "\n=== Fiyat İstatistikleri ===\n";
    std::cout << std::fixed << std::setprecision(5);
    std::cout << "Bid: Min=" << minBid << ", Max=" << maxBid << ", Avg=" << avgBid << std::endl;
    std::cout << "Ask: Min=" << minAsk << ", Max=" << maxAsk << ", Avg=" << avgAsk << std::endl;
    std::cout << "Spread: Min=" << minSpread << ", Max=" << maxSpread << ", Avg=" << avgSpread << " pips" << std::endl;
    
    // Pip değerlerini hesapla
    double pipsRange = (maxBid - minBid) * 10000;  // EUR/USD için 1 pip = 0.0001
    double avgSpreadPips = avgSpread * 10000;
    
    std::cout << "\n=== Pip İstatistikleri ===\n";
    std::cout << "Toplam fiyat aralığı: " << pipsRange << " pip" << std::endl;
    std::cout << "Ortalama spread: " << avgSpreadPips << " pip" << std::endl;
    
    // Fixed-Point analizi için öneriler
    std::cout << "\n=== Fixed-Point Bit Genişliği Önerileri ===\n";
    
    // Tamsayı kısmı için gereken bit sayısı (2 ^ N > maxValue)
    double maxValue = std::max(maxBid, maxAsk);
    int intBitsRequired = static_cast<int>(std::ceil(std::log2(maxValue))) + 1; // +1 işaret biti için
    
    // Kesirli kısım için gereken bit sayısı (2 ^ -N < minPipValue)
    double minPipValue = 0.0001;  // EUR/USD için 1 pip
    int fracBitsRequired = static_cast<int>(std::ceil(std::log2(1.0 / minPipValue)));
    
    std::cout << "Tamsayı kısmı için önerilen bit sayısı: " << intBitsRequired << " bit" << std::endl;
    std::cout << "Kesirli kısım için önerilen bit sayısı: " << fracBitsRequired << " bit" << std::endl;
    std::cout << "Önerilen FixedPoint konfigürasyonu: FixedPoint<" << intBitsRequired << ", " << fracBitsRequired << ">" << std::endl;
}

} // namespace gel::test