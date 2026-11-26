// include/gel/exit/stop_loss.hpp
#pragma once

#include <memory>
#include <optional>
#include <cmath>
#include <vector>
#include <algorithm>
#include "gel/position/position.hpp"

namespace gel::exit {

// Stop-Loss sonucu
struct StopLossResult {
    bool triggered = false;         // Stop-loss tetiklendi mi?
    double exit_price = 0.0;        // Cikis fiyati
    std::string reason;             // Aciklama
};

// Stop-Loss interface
class IStopLoss {
public:
    virtual ~IStopLoss() = default;

    // Pozisyon ve guncel fiyata gore stop-loss kontrolu
    [[nodiscard]] virtual StopLossResult check(
        const position::Position& pos,
        double current_price) const = 0;

    // Stop-loss seviyesini hesapla
    [[nodiscard]] virtual double calculate_level(
        const position::Position& pos) const = 0;

    // Stop-loss adini dondur
    [[nodiscard]] virtual std::string name() const = 0;
};

// ============================================================================
// 1. FIXED STOP-LOSS
// Sabit pip mesafesinde stop-loss
// ============================================================================
class FixedStopLoss : public IStopLoss {
public:
    explicit FixedStopLoss(double stop_pips)
        : stop_pips_(stop_pips) {}

    [[nodiscard]] StopLossResult check(
        const position::Position& pos,
        double current_price) const override {

        StopLossResult result;
        double sl_level = calculate_level(pos);

        if (pos.direction == position::Direction::LONG) {
            if (current_price <= sl_level) {
                result.triggered = true;
                result.exit_price = sl_level;
                result.reason = "Fixed SL hit at " + std::to_string(stop_pips_) + " pips";
            }
        } else {
            if (current_price >= sl_level) {
                result.triggered = true;
                result.exit_price = sl_level;
                result.reason = "Fixed SL hit at " + std::to_string(stop_pips_) + " pips";
            }
        }
        return result;
    }

    [[nodiscard]] double calculate_level(const position::Position& pos) const override {
        double pip_size = pos.pip_size();
        if (pos.direction == position::Direction::LONG) {
            return pos.entry_price - (stop_pips_ * pip_size);
        } else {
            return pos.entry_price + (stop_pips_ * pip_size);
        }
    }

    [[nodiscard]] std::string name() const override {
        return "FixedStopLoss(" + std::to_string(static_cast<int>(stop_pips_)) + " pips)";
    }

private:
    double stop_pips_;
};

// ============================================================================
// 2. ATR-BASED STOP-LOSS
// Volatiliteye gore dinamik stop-loss (ATR'nin X kati)
// ============================================================================
class ATRStopLoss : public IStopLoss {
public:
    // atr_multiplier: ATR'nin kac kati (genellikle 1.5-2.5)
    // current_atr: Suanki ATR degeri (pip cinsinden)
    ATRStopLoss(double atr_multiplier, double current_atr)
        : atr_multiplier_(atr_multiplier)
        , current_atr_(current_atr) {}

    // ATR'yi guncelle
    void update_atr(double new_atr) {
        current_atr_ = new_atr;
    }

    [[nodiscard]] StopLossResult check(
        const position::Position& pos,
        double current_price) const override {

        StopLossResult result;
        double sl_level = calculate_level(pos);

        if (pos.direction == position::Direction::LONG) {
            if (current_price <= sl_level) {
                result.triggered = true;
                result.exit_price = sl_level;
                result.reason = "ATR SL hit (" + std::to_string(atr_multiplier_) + "x ATR)";
            }
        } else {
            if (current_price >= sl_level) {
                result.triggered = true;
                result.exit_price = sl_level;
                result.reason = "ATR SL hit (" + std::to_string(atr_multiplier_) + "x ATR)";
            }
        }
        return result;
    }

    [[nodiscard]] double calculate_level(const position::Position& pos) const override {
        double pip_size = pos.pip_size();
        double stop_distance = current_atr_ * atr_multiplier_ * pip_size;

        if (pos.direction == position::Direction::LONG) {
            return pos.entry_price - stop_distance;
        } else {
            return pos.entry_price + stop_distance;
        }
    }

    [[nodiscard]] std::string name() const override {
        return "ATRStopLoss(" + std::to_string(atr_multiplier_) + "x)";
    }

    [[nodiscard]] double get_atr() const { return current_atr_; }
    [[nodiscard]] double get_multiplier() const { return atr_multiplier_; }

private:
    double atr_multiplier_;
    double current_atr_;
};

// ============================================================================
// 3. TRAILING STOP-LOSS
// Fiyat lehte gittikce stop-loss'u takip eder
// ============================================================================
class TrailingStopLoss : public IStopLoss {
public:
    // trailing_distance: Fiyattan kac pip geride kalacak
    // activation_profit: Trailing baslamasi icin gereken kar (pip) - opsiyonel
    TrailingStopLoss(double trailing_distance, double activation_profit = 0.0)
        : trailing_distance_(trailing_distance)
        , activation_profit_(activation_profit)
        , current_stop_level_(0.0)
        , is_activated_(false) {}

    // Trailing stop'u guncelle (her tick'te cagrilmali)
    void update(const position::Position& pos, double current_price) {
        double pip_size = pos.pip_size();
        double profit_pips = pos.unrealized_pnl_pips(current_price);

        // Aktivasyon kontrolu
        if (!is_activated_ && profit_pips >= activation_profit_) {
            is_activated_ = true;
            // Ilk stop seviyesini ayarla
            if (pos.direction == position::Direction::LONG) {
                current_stop_level_ = current_price - (trailing_distance_ * pip_size);
            } else {
                current_stop_level_ = current_price + (trailing_distance_ * pip_size);
            }
        }

        // Aktifse ve fiyat lehte gittiyse stop'u guncelle
        if (is_activated_) {
            if (pos.direction == position::Direction::LONG) {
                double new_stop = current_price - (trailing_distance_ * pip_size);
                if (new_stop > current_stop_level_) {
                    current_stop_level_ = new_stop;
                }
            } else {
                double new_stop = current_price + (trailing_distance_ * pip_size);
                if (new_stop < current_stop_level_) {
                    current_stop_level_ = new_stop;
                }
            }
        }
    }

    [[nodiscard]] StopLossResult check(
        const position::Position& pos,
        double current_price) const override {

        StopLossResult result;

        if (!is_activated_) {
            return result;  // Henuz aktif degil
        }

        if (pos.direction == position::Direction::LONG) {
            if (current_price <= current_stop_level_) {
                result.triggered = true;
                result.exit_price = current_stop_level_;
                result.reason = "Trailing SL hit at " + std::to_string(current_stop_level_);
            }
        } else {
            if (current_price >= current_stop_level_) {
                result.triggered = true;
                result.exit_price = current_stop_level_;
                result.reason = "Trailing SL hit at " + std::to_string(current_stop_level_);
            }
        }
        return result;
    }

    [[nodiscard]] double calculate_level(const position::Position& pos) const override {
        if (!is_activated_) {
            // Henuz aktif degilse giris fiyatindan hesapla
            double pip_size = pos.pip_size();
            if (pos.direction == position::Direction::LONG) {
                return pos.entry_price - (trailing_distance_ * pip_size);
            } else {
                return pos.entry_price + (trailing_distance_ * pip_size);
            }
        }
        return current_stop_level_;
    }

    [[nodiscard]] std::string name() const override {
        return "TrailingStopLoss(" + std::to_string(static_cast<int>(trailing_distance_)) + " pips)";
    }

    [[nodiscard]] bool is_active() const { return is_activated_; }
    [[nodiscard]] double get_current_level() const { return current_stop_level_; }

    // Reset (yeni pozisyon icin)
    void reset() {
        current_stop_level_ = 0.0;
        is_activated_ = false;
    }

private:
    double trailing_distance_;
    double activation_profit_;
    double current_stop_level_;
    bool is_activated_;
};

// ============================================================================
// 4. BREAKEVEN STOP-LOSS
// Belirli bir kar seviyesine ulasinca stop-loss'u girise ceker
// ============================================================================
class BreakevenStopLoss : public IStopLoss {
public:
    // trigger_profit: Breakeven'a gecirilmesi icin gereken kar (pip)
    // lock_profit: Giris fiyatinin ustunde/altinda kilitlenecek kar (pip) - opsiyonel
    BreakevenStopLoss(double trigger_profit, double lock_profit = 0.0)
        : trigger_profit_(trigger_profit)
        , lock_profit_(lock_profit)
        , is_at_breakeven_(false) {}

    // Breakeven'a ulasildiysa stop'u guncelle
    void update(const position::Position& pos, double current_price) {
        if (is_at_breakeven_) return;

        double profit_pips = pos.unrealized_pnl_pips(current_price);
        if (profit_pips >= trigger_profit_) {
            is_at_breakeven_ = true;
        }
    }

    [[nodiscard]] StopLossResult check(
        const position::Position& pos,
        double current_price) const override {

        StopLossResult result;

        if (!is_at_breakeven_) {
            return result;
        }

        double be_level = calculate_level(pos);

        if (pos.direction == position::Direction::LONG) {
            if (current_price <= be_level) {
                result.triggered = true;
                result.exit_price = be_level;
                result.reason = "Breakeven SL hit";
            }
        } else {
            if (current_price >= be_level) {
                result.triggered = true;
                result.exit_price = be_level;
                result.reason = "Breakeven SL hit";
            }
        }
        return result;
    }

    [[nodiscard]] double calculate_level(const position::Position& pos) const override {
        double pip_size = pos.pip_size();
        if (pos.direction == position::Direction::LONG) {
            return pos.entry_price + (lock_profit_ * pip_size);
        } else {
            return pos.entry_price - (lock_profit_ * pip_size);
        }
    }

    [[nodiscard]] std::string name() const override {
        return "BreakevenStopLoss(trigger:" + std::to_string(static_cast<int>(trigger_profit_)) + " pips)";
    }

    [[nodiscard]] bool is_breakeven() const { return is_at_breakeven_; }

    void reset() {
        is_at_breakeven_ = false;
    }

private:
    double trigger_profit_;
    double lock_profit_;
    bool is_at_breakeven_;
};

// ============================================================================
// STOP-LOSS FACTORY
// Stop-loss olusturmayi kolaylastiran factory
// ============================================================================
class StopLossFactory {
public:
    static std::unique_ptr<FixedStopLoss> createFixed(double pips) {
        return std::make_unique<FixedStopLoss>(pips);
    }

    static std::unique_ptr<ATRStopLoss> createATR(double multiplier, double atr) {
        return std::make_unique<ATRStopLoss>(multiplier, atr);
    }

    static std::unique_ptr<TrailingStopLoss> createTrailing(double distance, double activation = 0.0) {
        return std::make_unique<TrailingStopLoss>(distance, activation);
    }

    static std::unique_ptr<BreakevenStopLoss> createBreakeven(double trigger, double lock = 0.0) {
        return std::make_unique<BreakevenStopLoss>(trigger, lock);
    }
};

// ============================================================================
// ATR CALCULATOR
// ATR hesaplama yardimci sinifi
// ============================================================================
class ATRCalculator {
public:
    explicit ATRCalculator(int period = 14)
        : period_(period) {}

    // Yeni bir OHLC bar ekle ve ATR'yi guncelle
    void add_bar(double high, double low, double close) {
        if (previous_close_ == 0.0) {
            previous_close_ = close;
            return;
        }

        // True Range hesapla
        double tr1 = high - low;
        double tr2 = std::abs(high - previous_close_);
        double tr3 = std::abs(low - previous_close_);
        double true_range = std::max({tr1, tr2, tr3});

        true_ranges_.push_back(true_range);
        if (true_ranges_.size() > static_cast<size_t>(period_)) {
            true_ranges_.erase(true_ranges_.begin());
        }

        previous_close_ = close;

        // ATR hesapla (basit ortalama)
        if (true_ranges_.size() >= static_cast<size_t>(period_)) {
            double sum = 0.0;
            for (double tr : true_ranges_) {
                sum += tr;
            }
            current_atr_ = sum / period_;
        }
    }

    // ATR'yi pip cinsinden dondur
    [[nodiscard]] double get_atr_pips(double pip_size = 0.0001) const {
        return current_atr_ / pip_size;
    }

    [[nodiscard]] double get_atr() const { return current_atr_; }

    [[nodiscard]] bool is_ready() const {
        return true_ranges_.size() >= static_cast<size_t>(period_);
    }

private:
    int period_;
    double previous_close_ = 0.0;
    double current_atr_ = 0.0;
    std::vector<double> true_ranges_;
};

} // namespace gel::exit
