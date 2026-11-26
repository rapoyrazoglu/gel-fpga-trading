// src/scripts/create_sample_data.cpp
// Büyük Dukascopy dosyasından küçük bir örnek oluşturur
// Test'lerin hızlı çalışması için

#include <iostream>
#include <fstream>
#include <string>

int main(int argc, char* argv[]) {
    // Parametreler
    std::string input_file = "data/EURUSD_all_ticks_20250601_20251014.csv";
    std::string output_file = "data/EURUSD_sample.csv";
    int max_lines = 10000;  // 10K satır (test için yeterli)

    if (argc >= 2) {
        max_lines = std::atoi(argv[1]);
    }

    if (argc >= 3) {
        output_file = argv[2];
    }

    std::cout << "Creating sample data file...\n";
    std::cout << "Input: " << input_file << "\n";
    std::cout << "Output: " << output_file << "\n";
    std::cout << "Max lines: " << max_lines << "\n\n";

    // Giriş dosyasını aç
    std::ifstream input(input_file);
    if (!input.is_open()) {
        std::cerr << "Error: Could not open input file: " << input_file << std::endl;
        return 1;
    }

    // Çıkış dosyasını aç
    std::ofstream output(output_file);
    if (!output.is_open()) {
        std::cerr << "Error: Could not create output file: " << output_file << std::endl;
        return 1;
    }

    std::string line;
    int line_count = 0;

    // Header satırını kopyala
    if (std::getline(input, line)) {
        output << line << "\n";
        line_count++;
    }

    // İlk N satırı kopyala
    while (std::getline(input, line) && line_count < max_lines) {
        output << line << "\n";
        line_count++;

        if (line_count % 1000 == 0) {
            std::cout << "Copied " << line_count << " lines...\r" << std::flush;
        }
    }

    std::cout << "\nDone! Created sample file with " << line_count << " lines.\n";
    std::cout << "Sample file size: ";

    // Dosya boyutunu hesapla
    output.close();
    std::ifstream size_check(output_file, std::ios::ate | std::ios::binary);
    if (size_check.is_open()) {
        std::streamsize size = size_check.tellg();
        std::cout << (size / 1024.0) << " KB (" << (size / 1024.0 / 1024.0) << " MB)\n";
        size_check.close();
    }

    return 0;
}
