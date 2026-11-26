// include/gel/data/ohlc.hpp
#pragma once

#include <chrono>
#include <vector>
#include <optional>
#include "gel/data/tick.hpp"
#include "fixed_point.hpp"

namespace gel::data {

struct OHLCBar {
    std::chrono::system_clock::time_point timestamp;  // Bar başlangıç zamanı
    double open;
    double high;
    double low;
    double close;
    double volume;  // Toplam volume (bid_volume + ask_volume)

    // Hesaplanan değerler
    std::optional<double> typical_price;  // (high + low + close) / 3

    // FPGA uyumlu OHLC bar yapısı
    struct FPGAOHLCBar {
        uint64_t timestamp;
        fpga::FixedPoint<4, 20> open;
        fpga::FixedPoint<4, 20> high;
        fpga::FixedPoint<4, 20> low;
        fpga::FixedPoint<4, 20> close;
        fpga::FixedPoint<16, 16> volume;

        // Hesaplanan değerler
        [[nodiscard]] fpga::FixedPoint<4, 20> typical_price() const {
            return (high + low + close) / fpga::FixedPoint<4, 20>(3.0);
        }
    };

    // Default constructor
    OHLCBar() = default;

    // Tek bir tick'ten bar oluştur
    explicit OHLCBar(const Tick& tick) {
        timestamp = tick.timestamp;
        open = high = low = close = tick.mid_price();
        volume = tick.bid_volume + tick.ask_volume;
        typical_price = std::nullopt;
    }

    // Bar'ı yeni tick ile güncelle
    void update(const Tick& tick) {
        double mid = tick.mid_price();

        if (mid > high) high = mid;
        if (mid < low) low = mid;
        close = mid;
        volume += tick.bid_volume + tick.ask_volume;

        // Cache'i invalidate et
        typical_price = std::nullopt;
    }

    // Typical price hesapla ve cache'le
    [[nodiscard]] double get_typical_price() {
        if (!typical_price.has_value()) {
            typical_price = (high + low + close) / 3.0;
        }
        return typical_price.value();
    }

    // CPU -> FPGA dönüşümü
    [[nodiscard]] FPGAOHLCBar toFPGA() const {
        FPGAOHLCBar fpga_bar;

        // Timestamp: system_clock -> epoch milliseconds
        auto duration = timestamp.time_since_epoch();
        fpga_bar.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

        // Fiyatlar: double -> FixedPoint
        fpga_bar.open = fpga::FixedPoint<4, 20>(open);
        fpga_bar.high = fpga::FixedPoint<4, 20>(high);
        fpga_bar.low = fpga::FixedPoint<4, 20>(low);
        fpga_bar.close = fpga::FixedPoint<4, 20>(close);
        fpga_bar.volume = fpga::FixedPoint<16, 16>(volume);

        return fpga_bar;
    }

    // FPGA -> CPU dönüşümü
    static OHLCBar fromFPGA(const FPGAOHLCBar& fpga_bar) {
        OHLCBar bar;

        // Timestamp: epoch milliseconds -> system_clock
        bar.timestamp = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(fpga_bar.timestamp)
        );

        // Fiyatlar: FixedPoint -> double
        bar.open = static_cast<double>(fpga_bar.open);
        bar.high = static_cast<double>(fpga_bar.high);
        bar.low = static_cast<double>(fpga_bar.low);
        bar.close = static_cast<double>(fpga_bar.close);
        bar.volume = static_cast<double>(fpga_bar.volume);
        bar.typical_price = std::nullopt;

        return bar;
    }
};

// OHLC Builder sınıfı
class OHLCBuilder {
public:
    // Belirli bir timeframe için tick'leri OHLC'ye dönüştür
    static std::vector<OHLCBar> fromTicks(
            const std::vector<Tick>& ticks,
            std::chrono::minutes timeframe) {

        std::vector<OHLCBar> bars;
        if (ticks.empty()) return bars;

        // İlk bar'ı başlat
        auto bar_start_time = floorToTimeframe(ticks[0].timestamp, timeframe);
        OHLCBar current_bar(ticks[0]);
        current_bar.timestamp = bar_start_time;

        for (size_t i = 1; i < ticks.size(); ++i) {
            auto tick_bar_time = floorToTimeframe(ticks[i].timestamp, timeframe);

            if (tick_bar_time != bar_start_time) {
                // Yeni bar başladı, mevcut bar'ı kaydet
                bars.push_back(current_bar);

                // Yeni bar'ı başlat
                current_bar = OHLCBar(ticks[i]);
                current_bar.timestamp = tick_bar_time;
                bar_start_time = tick_bar_time;
            } else {
                // Aynı bar içinde, güncelle
                current_bar.update(ticks[i]);
            }
        }

        // Son bar'ı ekle
        bars.push_back(current_bar);

        return bars;
    }

    // Timestamp'i timeframe'e göre floor'la (en yakın geçmiş bar başlangıcı)
    static std::chrono::system_clock::time_point floorToTimeframe(
            std::chrono::system_clock::time_point timestamp,
            std::chrono::minutes timeframe) {

        auto duration = timestamp.time_since_epoch();
        auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration);

        // Floor to nearest timeframe
        auto floored_minutes = (minutes.count() / timeframe.count()) * timeframe.count();

        return std::chrono::system_clock::time_point(std::chrono::minutes(floored_minutes));
    }

    // Gerçek zamanlı işlem için
    class RealTimeBuilder {
    public:
        explicit RealTimeBuilder(std::chrono::minutes timeframe)
            : timeframe_(timeframe) {}

        // Yeni bir tick ekle ve eğer yeni bar tamamlandıysa döndür
        std::optional<OHLCBar> addTick(const Tick& tick) {
            auto tick_bar_time = OHLCBuilder::floorToTimeframe(tick.timestamp, timeframe_);

            if (!bar_initialized_) {
                // İlk tick
                current_bar_ = OHLCBar(tick);
                current_bar_.timestamp = tick_bar_time;
                current_bar_time_ = tick_bar_time;
                bar_initialized_ = true;
                return std::nullopt;
            }

            if (tick_bar_time != current_bar_time_) {
                // Yeni bar başladı
                OHLCBar completed_bar = current_bar_;

                // Yeni bar'ı başlat
                current_bar_ = OHLCBar(tick);
                current_bar_.timestamp = tick_bar_time;
                current_bar_time_ = tick_bar_time;

                return completed_bar;
            } else {
                // Aynı bar içinde
                current_bar_.update(tick);
                return std::nullopt;
            }
        }

        // Mevcut bar'ı al
        [[nodiscard]] const OHLCBar& getCurrentBar() const {
            return current_bar_;
        }

    private:
        std::chrono::minutes timeframe_;
        std::chrono::system_clock::time_point current_bar_time_;
        OHLCBar current_bar_;
        bool bar_initialized_ = false;
    };
};

} // namespace gel::data