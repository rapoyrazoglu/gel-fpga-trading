// include/gel/exit/time_exit.hpp
#pragma once

#include <memory>
#include <chrono>
#include <optional>
#include <string>
#include <cmath>
#include "gel/position/position.hpp"

namespace gel::exit {

// Time Exit sonucu
struct TimeExitResult {
    bool triggered = false;         // Zaman bazli cikis tetiklendi mi?
    std::string reason;             // Aciklama
};

// Time Exit interface
class ITimeExit {
public:
    virtual ~ITimeExit() = default;

    // Pozisyon ve guncel zamana gore kontrol
    [[nodiscard]] virtual TimeExitResult check(
        const position::Position& pos,
        std::chrono::system_clock::time_point current_time) const = 0;

    // Time exit adini dondur
    [[nodiscard]] virtual std::string name() const = 0;
};

// ============================================================================
// 1. MAX HOLD TIME EXIT
// Pozisyon belirli bir sureden fazla acik kalmissa kapat
// Jim Simons prensibi: "Zaman = Risk. Pozisyon ne kadar uzun aciksa o kadar riskli."
// ============================================================================
class MaxHoldTimeExit : public ITimeExit {
public:
    explicit MaxHoldTimeExit(std::chrono::minutes max_hold_time)
        : max_hold_time_(max_hold_time) {}

    // Saniye cinsinden de belirtilebilir
    explicit MaxHoldTimeExit(std::chrono::seconds max_hold_time)
        : max_hold_time_(std::chrono::duration_cast<std::chrono::minutes>(max_hold_time)) {
        // Saniyeyi de sakla (daha hassas kontrol icin)
        max_hold_seconds_ = max_hold_time;
    }

    [[nodiscard]] TimeExitResult check(
        const position::Position& pos,
        std::chrono::system_clock::time_point current_time) const override {

        TimeExitResult result;

        auto duration = current_time - pos.entry_time;

        // Saniye bazli kontrol (daha hassas)
        if (max_hold_seconds_.has_value()) {
            if (duration >= max_hold_seconds_.value()) {
                result.triggered = true;
                auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration).count();
                result.reason = "Max hold time exceeded (" + std::to_string(minutes) + " min)";
            }
        } else {
            // Dakika bazli kontrol
            if (duration >= max_hold_time_) {
                result.triggered = true;
                auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration).count();
                result.reason = "Max hold time exceeded (" + std::to_string(minutes) + " min)";
            }
        }

        return result;
    }

    [[nodiscard]] std::string name() const override {
        return "MaxHoldTimeExit(" + std::to_string(max_hold_time_.count()) + " min)";
    }

private:
    std::chrono::minutes max_hold_time_;
    std::optional<std::chrono::seconds> max_hold_seconds_;
};

// ============================================================================
// 2. INACTIVITY EXIT
// Fiyat belirli bir sure hareket etmemisse cik
// Jim Simons: "Bekledigimiz hareket gelmiyorsa, orada olmamaliyiz."
// ============================================================================
class InactivityExit : public ITimeExit {
public:
    // inactivity_timeout: Hareketsizlik suresi
    // min_movement_pips: Bu kadar pip hareket yoksa "hareketsiz" say
    InactivityExit(std::chrono::minutes inactivity_timeout, double min_movement_pips)
        : inactivity_timeout_(inactivity_timeout)
        , min_movement_pips_(min_movement_pips)
        , last_significant_move_time_()
        , last_price_(0.0)
        , is_initialized_(false) {}

    // Her tick'te guncelle
    void update(double current_price, std::chrono::system_clock::time_point current_time,
                double pip_size = 0.0001) {

        if (!is_initialized_) {
            last_price_ = current_price;
            last_significant_move_time_ = current_time;
            is_initialized_ = true;
            return;
        }

        // Fiyat degisimini hesapla (pip cinsinden)
        double price_change_pips = std::abs(current_price - last_price_) / pip_size;

        // Anlamli bir hareket olduysa zamani guncelle
        if (price_change_pips >= min_movement_pips_) {
            last_significant_move_time_ = current_time;
            last_price_ = current_price;
        }
    }

    [[nodiscard]] TimeExitResult check(
        const position::Position& pos,
        std::chrono::system_clock::time_point current_time) const override {

        TimeExitResult result;

        if (!is_initialized_) {
            return result;  // Henuz initialize edilmemis
        }

        auto duration_since_last_move = current_time - last_significant_move_time_;

        if (duration_since_last_move >= inactivity_timeout_) {
            result.triggered = true;
            auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration_since_last_move).count();
            result.reason = "Inactivity exit - no movement for " + std::to_string(minutes) + " min";
        }

        return result;
    }

    [[nodiscard]] std::string name() const override {
        return "InactivityExit(" + std::to_string(inactivity_timeout_.count()) + " min)";
    }

    void reset() {
        is_initialized_ = false;
        last_price_ = 0.0;
    }

private:
    std::chrono::minutes inactivity_timeout_;
    double min_movement_pips_;
    std::chrono::system_clock::time_point last_significant_move_time_;
    double last_price_;
    bool is_initialized_;
};

// ============================================================================
// 3. SESSION EXIT
// Belirli saat/seansta tum pozisyonlari kapat
// ============================================================================
class SessionExit : public ITimeExit {
public:
    // exit_hour: Kapatma saati (0-23, UTC)
    // exit_minute: Kapatma dakikasi (0-59)
    SessionExit(int exit_hour, int exit_minute = 0)
        : exit_hour_(exit_hour)
        , exit_minute_(exit_minute) {}

    [[nodiscard]] TimeExitResult check(
        const position::Position& /* pos */,
        std::chrono::system_clock::time_point current_time) const override {

        TimeExitResult result;

        // UTC saatini al
        auto time_t_now = std::chrono::system_clock::to_time_t(current_time);
        std::tm* utc_time = std::gmtime(&time_t_now);

        if (utc_time->tm_hour == exit_hour_ && utc_time->tm_min >= exit_minute_) {
            result.triggered = true;
            result.reason = "Session exit at " + std::to_string(exit_hour_) + ":" +
                           (exit_minute_ < 10 ? "0" : "") + std::to_string(exit_minute_) + " UTC";
        }

        return result;
    }

    [[nodiscard]] std::string name() const override {
        return "SessionExit(" + std::to_string(exit_hour_) + ":" +
               (exit_minute_ < 10 ? "0" : "") + std::to_string(exit_minute_) + " UTC)";
    }

    // Yaygın seans kapanış saatleri (UTC)
    static constexpr int NYC_CLOSE_HOUR = 21;      // New York 17:00 EST = 21:00 UTC (winter)
    static constexpr int LONDON_CLOSE_HOUR = 16;   // London 16:00 UTC
    static constexpr int TOKYO_CLOSE_HOUR = 6;     // Tokyo 15:00 JST = 06:00 UTC

private:
    int exit_hour_;
    int exit_minute_;
};

// ============================================================================
// 4. WEEKEND EXIT
// Cuma kapanişinda tum pozisyonlari kapat
// Forex piyasasi hafta sonu kapali - gap riski!
// ============================================================================
class WeekendExit : public ITimeExit {
public:
    // friday_close_hour: Cuma kapatma saati (UTC)
    // friday_close_minute: Cuma kapatma dakikasi
    explicit WeekendExit(int friday_close_hour = 21, int friday_close_minute = 0)
        : friday_close_hour_(friday_close_hour)
        , friday_close_minute_(friday_close_minute) {}

    [[nodiscard]] TimeExitResult check(
        const position::Position& /* pos */,
        std::chrono::system_clock::time_point current_time) const override {

        TimeExitResult result;

        auto time_t_now = std::chrono::system_clock::to_time_t(current_time);
        std::tm* utc_time = std::gmtime(&time_t_now);

        // Cuma = 5
        if (utc_time->tm_wday == 5) {
            if (utc_time->tm_hour > friday_close_hour_ ||
                (utc_time->tm_hour == friday_close_hour_ &&
                 utc_time->tm_min >= friday_close_minute_)) {
                result.triggered = true;
                result.reason = "Weekend exit - Friday close";
            }
        }
        // Cumartesi veya Pazar - hemen kapat (olmamasi gerekir ama yine de)
        else if (utc_time->tm_wday == 6 || utc_time->tm_wday == 0) {
            result.triggered = true;
            result.reason = "Weekend exit - market closed";
        }

        return result;
    }

    [[nodiscard]] std::string name() const override {
        return "WeekendExit(Friday " + std::to_string(friday_close_hour_) + ":" +
               (friday_close_minute_ < 10 ? "0" : "") + std::to_string(friday_close_minute_) + " UTC)";
    }

private:
    int friday_close_hour_;
    int friday_close_minute_;
};

// ============================================================================
// 5. NEWS EVENT EXIT
// Onemli haber oncesi pozisyonlari kapat
// ============================================================================
class NewsEventExit : public ITimeExit {
public:
    // minutes_before: Haberden kac dakika once kapatilacak
    NewsEventExit(std::chrono::system_clock::time_point news_time, int minutes_before = 15)
        : news_time_(news_time)
        , minutes_before_(minutes_before) {}

    [[nodiscard]] TimeExitResult check(
        const position::Position& /* pos */,
        std::chrono::system_clock::time_point current_time) const override {

        TimeExitResult result;

        auto time_until_news = news_time_ - current_time;
        auto minutes = std::chrono::duration_cast<std::chrono::minutes>(time_until_news).count();

        if (minutes <= minutes_before_ && minutes >= 0) {
            result.triggered = true;
            result.reason = "News event exit - " + std::to_string(minutes) + " min until news";
        }

        return result;
    }

    [[nodiscard]] std::string name() const override {
        return "NewsEventExit(" + std::to_string(minutes_before_) + " min before)";
    }

    void set_news_time(std::chrono::system_clock::time_point new_time) {
        news_time_ = new_time;
    }

private:
    std::chrono::system_clock::time_point news_time_;
    int minutes_before_;
};

// ============================================================================
// 6. PROFIT DECAY EXIT
// Kar belirli bir suredir dusuyorsa cik
// Jim Simons: "Momentum kaybolduysa, pozisyondan cik"
// ============================================================================
class ProfitDecayExit : public ITimeExit {
public:
    // decay_timeout: Kar dususunu tolere etme suresi
    // decay_threshold_pips: Bu kadar pip kar dususu olursa decay baslar
    ProfitDecayExit(std::chrono::minutes decay_timeout, double decay_threshold_pips)
        : decay_timeout_(decay_timeout)
        , decay_threshold_pips_(decay_threshold_pips)
        , peak_profit_pips_(0.0)
        , decay_start_time_()
        , is_decaying_(false) {}

    // Her tick'te guncelle
    void update(const position::Position& pos, double current_price,
                std::chrono::system_clock::time_point current_time) {

        double current_profit = pos.unrealized_pnl_pips(current_price);

        // Yeni peak mi?
        if (current_profit > peak_profit_pips_) {
            peak_profit_pips_ = current_profit;
            is_decaying_ = false;
        }
        // Kar dusus esigini gecti mi?
        else if (peak_profit_pips_ - current_profit >= decay_threshold_pips_) {
            if (!is_decaying_) {
                is_decaying_ = true;
                decay_start_time_ = current_time;
            }
        }
        // Decay esiginin altinda - decay durdu
        else {
            is_decaying_ = false;
        }
    }

    [[nodiscard]] TimeExitResult check(
        const position::Position& /* pos */,
        std::chrono::system_clock::time_point current_time) const override {

        TimeExitResult result;

        if (is_decaying_) {
            auto decay_duration = current_time - decay_start_time_;
            if (decay_duration >= decay_timeout_) {
                result.triggered = true;
                auto minutes = std::chrono::duration_cast<std::chrono::minutes>(decay_duration).count();
                result.reason = "Profit decay exit - profit declining for " +
                               std::to_string(minutes) + " min";
            }
        }

        return result;
    }

    [[nodiscard]] std::string name() const override {
        return "ProfitDecayExit(" + std::to_string(decay_timeout_.count()) + " min)";
    }

    void reset() {
        peak_profit_pips_ = 0.0;
        is_decaying_ = false;
    }

private:
    std::chrono::minutes decay_timeout_;
    double decay_threshold_pips_;
    double peak_profit_pips_;
    std::chrono::system_clock::time_point decay_start_time_;
    bool is_decaying_;
};

// ============================================================================
// TIME EXIT FACTORY
// ============================================================================
class TimeExitFactory {
public:
    static std::unique_ptr<MaxHoldTimeExit> createMaxHold(std::chrono::minutes duration) {
        return std::make_unique<MaxHoldTimeExit>(duration);
    }

    static std::unique_ptr<InactivityExit> createInactivity(
        std::chrono::minutes timeout, double min_pips) {
        return std::make_unique<InactivityExit>(timeout, min_pips);
    }

    static std::unique_ptr<SessionExit> createSession(int hour, int minute = 0) {
        return std::make_unique<SessionExit>(hour, minute);
    }

    static std::unique_ptr<WeekendExit> createWeekend(int friday_hour = 21, int friday_minute = 0) {
        return std::make_unique<WeekendExit>(friday_hour, friday_minute);
    }

    // Jim Simons tarzi: Kisa sureli pozisyonlar
    static std::unique_ptr<MaxHoldTimeExit> createJimSimonsStyle() {
        // Maksimum 30 dakika pozisyon tut
        return std::make_unique<MaxHoldTimeExit>(std::chrono::minutes(30));
    }

    // NYC Close (en yaygin forex kapanis)
    static std::unique_ptr<SessionExit> createNYCClose() {
        return std::make_unique<SessionExit>(SessionExit::NYC_CLOSE_HOUR, 0);
    }
};

} // namespace gel::exit
