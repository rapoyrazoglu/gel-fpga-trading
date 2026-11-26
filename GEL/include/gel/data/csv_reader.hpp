#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace gel::data {

/**
 * @brief CSV dosyalarını okumak için yardımcı sınıf
 */
class CSVReader {
public:
    /**
     * @brief Dosyadan tüm satırları okur
     * @param filename CSV dosya yolu
     * @return Satırların vektörü
     */
    static std::vector<std::string> readLines(const std::string& filename) {
        std::vector<std::string> lines;
        std::ifstream file(filename);

        if (!file.is_open()) {
            throw std::runtime_error("Could not open file: " + filename);
        }

        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(line);
        }

        return lines;
    }

    /**
     * @brief CSV dosyasını parse eder ve 2D vector olarak döndürür
     * @param filename CSV dosya yolu
     * @param delimiter Ayırıcı karakter (default: ',')
     * @param hasHeader Header satırı var mı (default: true)
     * @return Alanların 2D vektörü
     */
    static std::vector<std::vector<std::string>> parseCSV(
            const std::string& filename,
            char delimiter = ',',
            bool hasHeader = true,
            size_t maxRows = 0) {

        std::vector<std::vector<std::string>> data;
        std::ifstream file(filename);

        if (!file.is_open()) {
            throw std::runtime_error("Could not open file: " + filename);
        }

        std::string line;

        // Header satırını atla
        if (hasHeader && std::getline(file, line)) {
            // İstenirse header'ı da ekle
            // data.push_back(splitLine(line, delimiter));
        }

        // Geri kalan satırları işle
        size_t rowCount = 0;
        while (std::getline(file, line) && (maxRows == 0 || rowCount < maxRows)) {
            data.push_back(splitLine(line, delimiter));
            rowCount++;
        }
        return data;
    }

    /**
     * @brief CSV'deki header satırını döndürür
     * @param filename CSV dosya yolu
     * @param delimiter Ayırıcı karakter (default: ',')
     * @return Header alanlarının vektörü
     */
    static std::vector<std::string> getHeader(
            const std::string& filename,
            char delimiter = ',') {

        std::ifstream file(filename);

        if (!file.is_open()) {
            throw std::runtime_error("Could not open file: " + filename);
        }

        std::string line;
        if (std::getline(file, line)) {
            return splitLine(line, delimiter);
        }

        return {};
    }

    /**
     * @brief Bir CSV dosyasının satır sayısını döndürür
     * @param filename CSV dosya yolu
     * @return Satır sayısı
     */
    static size_t getRowCount(const std::string& filename) {
        std::ifstream file(filename);

        if (!file.is_open()) {
            throw std::runtime_error("Could not open file: " + filename);
        }

        size_t count = 0;
        std::string line;

        while (std::getline(file, line)) {
            count++;
        }

        return count;
    }

private:
    /**
     * @brief Bir satırı belirtilen ayırıcıya göre böler
     * @param line Bölünecek satır
     * @param delimiter Ayırıcı karakter
     * @return Alanların vektörü
     */
    static std::vector<std::string> splitLine(const std::string& line, char delimiter) {
        std::vector<std::string> tokens;
        std::stringstream ss(line);
        std::string token;

        while (std::getline(ss, token, delimiter)) {
            // Baştaki ve sondaki boşlukları temizle
            token.erase(0, token.find_first_not_of(" \t\r\n"));
            token.erase(token.find_last_not_of(" \t\r\n") + 1);

            tokens.push_back(token);
        }

        return tokens;
    }
};

} // namespace gel::data