#include <fstream>
#include <string>
#include <iostream>

int main() {
    const std::string input_path = "data/EURUSD_all_ticks_20250601_20251014_deneme.csv";
    const std::string output_path = "data/EURUSD_all_ticks_20250601_20251014_deneme_temiz.csv";

    std::ifstream infile(input_path);
    if (!infile.is_open()) {
        std::cerr << "❌ Hata: Girdi dosyasi acilamadi -> " << input_path << std::endl;
        return 1;
    }

    std::ofstream outfile(output_path);
    if (!outfile.is_open()) {
        std::cerr << "❌ Hata: Cikti dosyasi olusturulamadi -> " << output_path << std::endl;
        return 1;
    }

    std::string line;
    long long line_num = 0;
    long long deleted_count = 0;

    while (std::getline(infile, line)) {
        ++line_num;
        if (line_num == 1 || line_num > 6762279) {
            outfile << line << "\n";
        } else {
            ++deleted_count;
            // Her 1 milyon satırda bir bilgi mesajı yazalım (konsol aşırı dolmasın)
            if (deleted_count % 1000000 == 0)
                std::cout << "🗑️  " << deleted_count << " satir silindi..." << std::endl;
        }
    }

    std::cout << "✅ Islem tamamlandi. Toplam " << deleted_count
              << " satir silindi.\nYeni dosya: " << output_path << std::endl;

    infile.close();
    outfile.close();

    return 0;
}
