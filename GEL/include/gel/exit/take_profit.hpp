// include/gel/exit/take_profit.hpp
#pragma once

#include <memory>
#include <optional>
#include <vector>
#include <cmath>
#include "gel/position/position.hpp"

namespace gel::exit {

// Take-Profit sonucu
struct TakeProfitResult {
    bool triggered = false;         // Take-profit tetiklendi mi?
    double exit_price = 0.0;        // Cikis fiyati
    double exit_percentage = 100.0; // Cikilacak pozisyon yuzdeligi (partial TP icin)
    std::string reason;             // Aciklama
};

// Take-Profit interface
class ITakeProfit {
public:
    virtual ~ITakeProfit() = default;

    // Pozisyon ve guncel fiyata gore take-profit kontrolu
    [[nodiscard]] virtual TakeProfitResult check(
        const position::Position& pos,
        double current_price) const = 0;

    // Take-profit seviyesini hesapla
    [[nodiscard]] virtual double calculate_level(
        const position::Position& pos) const = 0;

    // Take-profit adini dondur
    [[nodiscard]] virtual std::string name() const = 0;
};

// ============================================================================
// 1. FIXED TAKE-PROFIT
// Sabit pip mesafesinde take-profit
// ============================================================================
class FixedTakeProfit : public ITakeProfit {
public:
    explicit FixedTakeProfit(double target_pips)
        : target_pips_(target_pips) {}

    [[nodiscard]] TakeProfitResult check(
        const position::Position& pos,
        double current_price) const override {

        TakeProfitResult result;
        double tp_level = calculate_level(pos);

        if (pos.direction == position::Direction::LONG) {
            if (current_price >= tp_level) {
                result.triggered = true;
                result.exit_price = tp_level;
                result.reason = "Fixed TP hit at " + std::to_string(target_pips_) + " pips";
            }
        } else {
            if (current_price <= tp_level) {
                result.triggered = true;
                result.exit_price = tp_level;
                result.reason = "Fixed TP hit at " + std::to_string(target_pips_) + " pips";
            }
        }
        return result;
    }

    [[nodiscard]] double calculate_level(const position::Position& pos) const override {
        double pip_size = pos.pip_size();
        if (pos.direction == position::Direction::LONG) {
            return pos.entry_price + (target_pips_ * pip_size);
        } else {
            return pos.entry_price - (target_pips_ * pip_size);
        }
    }

    [[nodiscard]] std::string name() const override {
        return "FixedTakeProfit(" + std::to_string(static_cast<int>(target_pips_)) + " pips)";
    }

private:
    double target_pips_;
};

// ============================================================================
// 2. RISK/REWARD BASED TAKE-PROFIT
// Stop-loss'un X kati mesafede take-profit (orn: 1.5R, 2R)
// ============================================================================
class RiskRewardTakeProfit : public ITakeProfit {
public:
    // risk_reward_ratio: Ornegin 1.5 demek SL'nin 1.5 kati kar hedefi
    // stop_loss_pips: Mevcut stop-loss mesafesi (pip)
    RiskRewardTakeProfit(double risk_reward_ratio, double stop_loss_pips)
        : rr_ratio_(risk_reward_ratio)
        , sl_pips_(stop_loss_pips) {}

    // Stop-loss mesafesini guncelle
    void update_stop_loss(double new_sl_pips) {
        sl_pips_ = new_sl_pips;
    }

    [[nodiscard]] TakeProfitResult check(
        const position::Position& pos,
        double current_price) const override {

        TakeProfitResult result;
        double tp_level = calculate_level(pos);

        if (pos.direction == position::Direction::LONG) {
            if (current_price >= tp_level) {
                result.triggered = true;
                result.exit_price = tp_level;
                result.reason = "R:R TP hit at " + std::to_string(rr_ratio_) + "R";
            }
        } else {
            if (current_price <= tp_level) {
                result.triggered = true;
                result.exit_price = tp_level;
                result.reason = "R:R TP hit at " + std::to_string(rr_ratio_) + "R";
            }
        }
        return result;
    }

    [[nodiscard]] double calculate_level(const position::Position& pos) const override {
        double pip_size = pos.pip_size();
        double target_pips = sl_pips_ * rr_ratio_;

        if (pos.direction == position::Direction::LONG) {
            return pos.entry_price + (target_pips * pip_size);
        } else {
            return pos.entry_price - (target_pips * pip_size);
        }
    }

    [[nodiscard]] std::string name() const override {
        return "RiskRewardTakeProfit(" + std::to_string(rr_ratio_) + "R)";
    }

    [[nodiscard]] double get_ratio() const { return rr_ratio_; }

private:
    double rr_ratio_;
    double sl_pips_;
};

// ============================================================================
// 3. PARTIAL TAKE-PROFIT
// Kademeli kar alma (orn: %50 @ 1R, %50 @ 2R)
// ============================================================================
struct PartialTarget {
    double profit_pips;      // Bu seviyeye ulasinca
    double exit_percentage;  // Bu kadar pozisyon kapat
};

class PartialTakeProfit : public ITakeProfit {
public:
    PartialTakeProfit() = default;

    // Hedef ekle
    void add_target(double profit_pips, double exit_percentage) {
        targets_.push_back({profit_pips, exit_percentage});
    }

    // Hedefe ulasildiysa isaretleme
    void mark_target_hit(size_t index) {
        if (index < hit_targets_.size()) {
            hit_targets_[index] = true;
        }
    }

    [[nodiscard]] TakeProfitResult check(
        const position::Position& pos,
        double current_price) const override {

        TakeProfitResult result;

        // Henuz hit edilmemis en yakin hedefi bul
        for (size_t i = 0; i < targets_.size(); ++i) {
            if (i < hit_targets_.size() && hit_targets_[i]) continue;

            double pip_size = pos.pip_size();
            double tp_level;

            if (pos.direction == position::Direction::LONG) {
                tp_level = pos.entry_price + (targets_[i].profit_pips * pip_size);
                if (current_price >= tp_level) {
                    result.triggered = true;
                    result.exit_price = tp_level;
                    result.exit_percentage = targets_[i].exit_percentage;
                    result.reason = "Partial TP #" + std::to_string(i + 1) +
                                    " hit (" + std::to_string(targets_[i].exit_percentage) + "%)";
                    return result;
                }
            } else {
                tp_level = pos.entry_price - (targets_[i].profit_pips * pip_size);
                if (current_price <= tp_level) {
                    result.triggered = true;
                    result.exit_price = tp_level;
                    result.exit_percentage = targets_[i].exit_percentage;
                    result.reason = "Partial TP #" + std::to_string(i + 1) +
                                    " hit (" + std::to_string(targets_[i].exit_percentage) + "%)";
                    return result;
                }
            }
        }

        return result;
    }

    [[nodiscard]] double calculate_level(const position::Position& pos) const override {
        // Ilk hit edilmemis hedefi dondur
        for (size_t i = 0; i < targets_.size(); ++i) {
            if (i < hit_targets_.size() && hit_targets_[i]) continue;

            double pip_size = pos.pip_size();
            if (pos.direction == position::Direction::LONG) {
                return pos.entry_price + (targets_[i].profit_pips * pip_size);
            } else {
                return pos.entry_price - (targets_[i].profit_pips * pip_size);
            }
        }
        return pos.entry_price;  // Tum hedefler tamamlanmis
    }

    [[nodiscard]] std::string name() const override {
        return "PartialTakeProfit(" + std::to_string(targets_.size()) + " targets)";
    }

    [[nodiscard]] const std::vector<PartialTarget>& get_targets() const { return targets_; }

    void reset() {
        hit_targets_.clear();
        hit_targets_.resize(targets_.size(), false);
    }

private:
    std::vector<PartialTarget> targets_;
    mutable std::vector<bool> hit_targets_;
};

// ============================================================================
// 4. FIBONACCI TAKE-PROFIT
// Fibonacci seviyelerine gore take-profit (swing high/low'dan hesaplanan)
// ============================================================================
class FibonacciTakeProfit : public ITakeProfit {
public:
    // swing_size: Swing boyutu (pip cinsinden)
    // fib_level: Hedef Fibonacci seviyesi (0.618, 1.0, 1.272, 1.618, 2.618)
    FibonacciTakeProfit(double swing_size, double fib_level = 1.0)
        : swing_size_(swing_size)
        , fib_level_(fib_level) {}

    [[nodiscard]] TakeProfitResult check(
        const position::Position& pos,
        double current_price) const override {

        TakeProfitResult result;
        double tp_level = calculate_level(pos);

        if (pos.direction == position::Direction::LONG) {
            if (current_price >= tp_level) {
                result.triggered = true;
                result.exit_price = tp_level;
                result.reason = "Fibonacci TP hit at " + std::to_string(fib_level_ * 100) + "%";
            }
        } else {
            if (current_price <= tp_level) {
                result.triggered = true;
                result.exit_price = tp_level;
                result.reason = "Fibonacci TP hit at " + std::to_string(fib_level_ * 100) + "%";
            }
        }
        return result;
    }

    [[nodiscard]] double calculate_level(const position::Position& pos) const override {
        double pip_size = pos.pip_size();
        double target_pips = swing_size_ * fib_level_;

        if (pos.direction == position::Direction::LONG) {
            return pos.entry_price + (target_pips * pip_size);
        } else {
            return pos.entry_price - (target_pips * pip_size);
        }
    }

    [[nodiscard]] std::string name() const override {
        return "FibonacciTakeProfit(" + std::to_string(static_cast<int>(fib_level_ * 100)) + "%)";
    }

    // Yaygın Fibonacci seviyeleri
    static constexpr double FIB_382 = 0.382;
    static constexpr double FIB_500 = 0.500;
    static constexpr double FIB_618 = 0.618;
    static constexpr double FIB_100 = 1.000;
    static constexpr double FIB_127 = 1.272;
    static constexpr double FIB_161 = 1.618;
    static constexpr double FIB_261 = 2.618;

private:
    double swing_size_;
    double fib_level_;
};

// ============================================================================
// 5. DYNAMIC TAKE-PROFIT
// Piyasa kosullarina gore dinamik ayarlanan TP
// ============================================================================
class DynamicTakeProfit : public ITakeProfit {
public:
    // base_target: Temel hedef (pip)
    // volatility_factor: Volatilite carpani
    DynamicTakeProfit(double base_target, double volatility_factor = 1.0)
        : base_target_(base_target)
        , volatility_factor_(volatility_factor)
        , current_volatility_(1.0) {}

    // Volatiliteyi guncelle (ATR veya baska bir olcum)
    void update_volatility(double new_volatility) {
        current_volatility_ = new_volatility;
    }

    [[nodiscard]] TakeProfitResult check(
        const position::Position& pos,
        double current_price) const override {

        TakeProfitResult result;
        double tp_level = calculate_level(pos);

        if (pos.direction == position::Direction::LONG) {
            if (current_price >= tp_level) {
                result.triggered = true;
                result.exit_price = tp_level;
                result.reason = "Dynamic TP hit";
            }
        } else {
            if (current_price <= tp_level) {
                result.triggered = true;
                result.exit_price = tp_level;
                result.reason = "Dynamic TP hit";
            }
        }
        return result;
    }

    [[nodiscard]] double calculate_level(const position::Position& pos) const override {
        double pip_size = pos.pip_size();
        double adjusted_target = base_target_ * current_volatility_ * volatility_factor_;

        if (pos.direction == position::Direction::LONG) {
            return pos.entry_price + (adjusted_target * pip_size);
        } else {
            return pos.entry_price - (adjusted_target * pip_size);
        }
    }

    [[nodiscard]] std::string name() const override {
        return "DynamicTakeProfit";
    }

private:
    double base_target_;
    double volatility_factor_;
    double current_volatility_;
};

// ============================================================================
// TAKE-PROFIT FACTORY
// Take-profit olusturmayi kolaylastiran factory
// ============================================================================
class TakeProfitFactory {
public:
    static std::unique_ptr<FixedTakeProfit> createFixed(double pips) {
        return std::make_unique<FixedTakeProfit>(pips);
    }

    static std::unique_ptr<RiskRewardTakeProfit> createRiskReward(double ratio, double sl_pips) {
        return std::make_unique<RiskRewardTakeProfit>(ratio, sl_pips);
    }

    static std::unique_ptr<PartialTakeProfit> createPartial(
        const std::vector<std::pair<double, double>>& targets) {
        auto tp = std::make_unique<PartialTakeProfit>();
        for (const auto& [pips, pct] : targets) {
            tp->add_target(pips, pct);
        }
        return tp;
    }

    static std::unique_ptr<FibonacciTakeProfit> createFibonacci(
        double swing_size, double fib_level = 1.0) {
        return std::make_unique<FibonacciTakeProfit>(swing_size, fib_level);
    }

    // Jim Simons tarzi: Dusuk hedef, yuksek frekansta islem
    static std::unique_ptr<RiskRewardTakeProfit> createJimSimonsStyle(double sl_pips) {
        // 1.5R - kucuk ama tutarli kar hedefi
        return std::make_unique<RiskRewardTakeProfit>(1.5, sl_pips);
    }
};

} // namespace gel::exit
