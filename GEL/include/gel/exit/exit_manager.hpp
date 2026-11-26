// include/gel/exit/exit_manager.hpp
#pragma once

#include <memory>
#include <vector>
#include <optional>
#include <string>
#include <chrono>
#include <functional>
#include "gel/position/position.hpp"
#include "gel/exit/stop_loss.hpp"
#include "gel/exit/take_profit.hpp"
#include "gel/exit/time_exit.hpp"

namespace gel::exit {

// Cikis sinyali
struct ExitSignal {
    bool should_exit = false;
    double exit_price = 0.0;
    double exit_percentage = 100.0;  // Partial exit icin
    position::ExitReason reason = position::ExitReason::NONE;
    std::string description;
    int priority = 0;  // Dusuk numara = yuksek oncelik

    // Karsilastirma operatoru (oncelik siralamasi icin)
    bool operator<(const ExitSignal& other) const {
        return priority < other.priority;
    }
};

// Exit istatistikleri
struct ExitStatistics {
    int total_exits = 0;
    int stop_loss_exits = 0;
    int take_profit_exits = 0;
    int trailing_stop_exits = 0;
    int time_exits = 0;
    int manual_exits = 0;

    // Exit reason'a gore say
    void record_exit(position::ExitReason reason) {
        total_exits++;
        switch (reason) {
            case position::ExitReason::STOP_LOSS:
                stop_loss_exits++;
                break;
            case position::ExitReason::TAKE_PROFIT:
                take_profit_exits++;
                break;
            case position::ExitReason::TRAILING_STOP:
                trailing_stop_exits++;
                break;
            case position::ExitReason::TIME_EXIT:
            case position::ExitReason::SESSION_CLOSE:
                time_exits++;
                break;
            case position::ExitReason::MANUAL:
                manual_exits++;
                break;
            default:
                break;
        }
    }

    // Oranlari hesapla
    [[nodiscard]] double stop_loss_rate() const {
        return total_exits > 0 ? (static_cast<double>(stop_loss_exits) / total_exits) * 100.0 : 0.0;
    }

    [[nodiscard]] double take_profit_rate() const {
        return total_exits > 0 ? (static_cast<double>(take_profit_exits) / total_exits) * 100.0 : 0.0;
    }

    [[nodiscard]] double time_exit_rate() const {
        return total_exits > 0 ? (static_cast<double>(time_exits) / total_exits) * 100.0 : 0.0;
    }
};

// ============================================================================
// EXIT MANAGER
// Tum exit sistemlerini koordine eder
// ============================================================================
class ExitManager {
public:
    ExitManager() = default;

    // Stop-Loss ekle
    void add_stop_loss(std::unique_ptr<IStopLoss> sl) {
        stop_losses_.push_back(std::move(sl));
    }

    // Take-Profit ekle
    void add_take_profit(std::unique_ptr<ITakeProfit> tp) {
        take_profits_.push_back(std::move(tp));
    }

    // Time Exit ekle
    void add_time_exit(std::unique_ptr<ITimeExit> te) {
        time_exits_.push_back(std::move(te));
    }

    // Trailing Stop ekle (ozel durum - update gerektirir)
    void set_trailing_stop(std::unique_ptr<TrailingStopLoss> ts) {
        trailing_stop_ = std::move(ts);
    }

    // Breakeven Stop ekle (ozel durum - update gerektirir)
    void set_breakeven_stop(std::unique_ptr<BreakevenStopLoss> be) {
        breakeven_stop_ = std::move(be);
    }

    // Inactivity Exit ekle (ozel durum - update gerektirir)
    void set_inactivity_exit(std::unique_ptr<InactivityExit> ie) {
        inactivity_exit_ = std::move(ie);
    }

    // Profit Decay Exit ekle (ozel durum - update gerektirir)
    void set_profit_decay_exit(std::unique_ptr<ProfitDecayExit> pde) {
        profit_decay_exit_ = std::move(pde);
    }

    // Dinamik sistemleri guncelle (her tick'te cagrilmali)
    void update(const position::Position& pos, double current_price,
                std::chrono::system_clock::time_point current_time,
                double pip_size = 0.0001) {

        // Trailing stop guncelle
        if (trailing_stop_) {
            trailing_stop_->update(pos, current_price);
        }

        // Breakeven guncelle
        if (breakeven_stop_) {
            breakeven_stop_->update(pos, current_price);
        }

        // Inactivity guncelle
        if (inactivity_exit_) {
            inactivity_exit_->update(current_price, current_time, pip_size);
        }

        // Profit decay guncelle
        if (profit_decay_exit_) {
            profit_decay_exit_->update(pos, current_price, current_time);
        }
    }

    // Ana kontrol fonksiyonu - cikis sinyali varsa dondur
    [[nodiscard]] ExitSignal check_exit(
        const position::Position& pos,
        double current_price,
        std::chrono::system_clock::time_point current_time) const {

        std::vector<ExitSignal> signals;

        // 1. Stop-Loss kontrolleri (en yuksek oncelik)
        for (const auto& sl : stop_losses_) {
            auto result = sl->check(pos, current_price);
            if (result.triggered) {
                ExitSignal signal;
                signal.should_exit = true;
                signal.exit_price = result.exit_price;
                signal.reason = position::ExitReason::STOP_LOSS;
                signal.description = result.reason;
                signal.priority = 1;  // En yuksek oncelik
                signals.push_back(signal);
            }
        }

        // 2. Trailing Stop kontrolu
        if (trailing_stop_) {
            auto result = trailing_stop_->check(pos, current_price);
            if (result.triggered) {
                ExitSignal signal;
                signal.should_exit = true;
                signal.exit_price = result.exit_price;
                signal.reason = position::ExitReason::TRAILING_STOP;
                signal.description = result.reason;
                signal.priority = 2;
                signals.push_back(signal);
            }
        }

        // 3. Breakeven Stop kontrolu
        if (breakeven_stop_) {
            auto result = breakeven_stop_->check(pos, current_price);
            if (result.triggered) {
                ExitSignal signal;
                signal.should_exit = true;
                signal.exit_price = result.exit_price;
                signal.reason = position::ExitReason::STOP_LOSS;
                signal.description = result.reason;
                signal.priority = 2;
                signals.push_back(signal);
            }
        }

        // 4. Take-Profit kontrolleri
        for (const auto& tp : take_profits_) {
            auto result = tp->check(pos, current_price);
            if (result.triggered) {
                ExitSignal signal;
                signal.should_exit = true;
                signal.exit_price = result.exit_price;
                signal.exit_percentage = result.exit_percentage;
                signal.reason = position::ExitReason::TAKE_PROFIT;
                signal.description = result.reason;
                signal.priority = 3;
                signals.push_back(signal);
            }
        }

        // 5. Time Exit kontrolleri
        for (const auto& te : time_exits_) {
            auto result = te->check(pos, current_time);
            if (result.triggered) {
                ExitSignal signal;
                signal.should_exit = true;
                signal.exit_price = current_price;  // Market price
                signal.reason = position::ExitReason::TIME_EXIT;
                signal.description = result.reason;
                signal.priority = 4;
                signals.push_back(signal);
            }
        }

        // 6. Inactivity Exit kontrolu
        if (inactivity_exit_) {
            auto result = inactivity_exit_->check(pos, current_time);
            if (result.triggered) {
                ExitSignal signal;
                signal.should_exit = true;
                signal.exit_price = current_price;
                signal.reason = position::ExitReason::TIME_EXIT;
                signal.description = result.reason;
                signal.priority = 5;
                signals.push_back(signal);
            }
        }

        // 7. Profit Decay Exit kontrolu
        if (profit_decay_exit_) {
            auto result = profit_decay_exit_->check(pos, current_time);
            if (result.triggered) {
                ExitSignal signal;
                signal.should_exit = true;
                signal.exit_price = current_price;
                signal.reason = position::ExitReason::TIME_EXIT;
                signal.description = result.reason;
                signal.priority = 5;
                signals.push_back(signal);
            }
        }

        // En yuksek oncelikli sinyali dondur
        if (!signals.empty()) {
            std::sort(signals.begin(), signals.end());
            return signals.front();
        }

        return ExitSignal{};
    }

    // Callback fonksiyonu - exit oldugunda cagrilir
    void set_exit_callback(std::function<void(const position::Position&, const ExitSignal&)> callback) {
        exit_callback_ = std::move(callback);
    }

    // Pozisyonu kapat ve callback'i cagir
    void execute_exit(position::Position& pos, const ExitSignal& signal,
                      std::chrono::system_clock::time_point current_time) {

        if (!signal.should_exit) return;

        pos.close(signal.exit_price, current_time, signal.reason);
        statistics_.record_exit(signal.reason);

        if (exit_callback_) {
            exit_callback_(pos, signal);
        }
    }

    // Istatistikleri dondur
    [[nodiscard]] const ExitStatistics& get_statistics() const {
        return statistics_;
    }

    // Tum dinamik exit'leri resetle (yeni pozisyon icin)
    void reset_dynamic_exits() {
        if (trailing_stop_) trailing_stop_->reset();
        if (breakeven_stop_) breakeven_stop_->reset();
        if (inactivity_exit_) inactivity_exit_->reset();
        if (profit_decay_exit_) profit_decay_exit_->reset();
    }

    // Sistemleri temizle
    void clear() {
        stop_losses_.clear();
        take_profits_.clear();
        time_exits_.clear();
        trailing_stop_.reset();
        breakeven_stop_.reset();
        inactivity_exit_.reset();
        profit_decay_exit_.reset();
    }

    // Bilgi metodlari
    [[nodiscard]] size_t stop_loss_count() const { return stop_losses_.size(); }
    [[nodiscard]] size_t take_profit_count() const { return take_profits_.size(); }
    [[nodiscard]] size_t time_exit_count() const { return time_exits_.size(); }
    [[nodiscard]] bool has_trailing_stop() const { return trailing_stop_ != nullptr; }
    [[nodiscard]] bool has_breakeven_stop() const { return breakeven_stop_ != nullptr; }

private:
    std::vector<std::unique_ptr<IStopLoss>> stop_losses_;
    std::vector<std::unique_ptr<ITakeProfit>> take_profits_;
    std::vector<std::unique_ptr<ITimeExit>> time_exits_;

    // Dinamik exit'ler (update gerektirir)
    std::unique_ptr<TrailingStopLoss> trailing_stop_;
    std::unique_ptr<BreakevenStopLoss> breakeven_stop_;
    std::unique_ptr<InactivityExit> inactivity_exit_;
    std::unique_ptr<ProfitDecayExit> profit_decay_exit_;

    std::function<void(const position::Position&, const ExitSignal&)> exit_callback_;
    ExitStatistics statistics_;
};

// ============================================================================
// EXIT MANAGER BUILDER
// Fluent API ile kolay konfigürasyon
// ============================================================================
class ExitManagerBuilder {
public:
    ExitManagerBuilder() : manager_(std::make_unique<ExitManager>()) {}

    // Stop-Loss ekle
    ExitManagerBuilder& with_fixed_stop_loss(double pips) {
        manager_->add_stop_loss(StopLossFactory::createFixed(pips));
        return *this;
    }

    ExitManagerBuilder& with_atr_stop_loss(double multiplier, double atr) {
        manager_->add_stop_loss(StopLossFactory::createATR(multiplier, atr));
        return *this;
    }

    ExitManagerBuilder& with_trailing_stop(double distance, double activation = 0.0) {
        manager_->set_trailing_stop(StopLossFactory::createTrailing(distance, activation));
        return *this;
    }

    ExitManagerBuilder& with_breakeven_stop(double trigger, double lock = 0.0) {
        manager_->set_breakeven_stop(StopLossFactory::createBreakeven(trigger, lock));
        return *this;
    }

    // Take-Profit ekle
    ExitManagerBuilder& with_fixed_take_profit(double pips) {
        manager_->add_take_profit(TakeProfitFactory::createFixed(pips));
        return *this;
    }

    ExitManagerBuilder& with_risk_reward_take_profit(double ratio, double sl_pips) {
        manager_->add_take_profit(TakeProfitFactory::createRiskReward(ratio, sl_pips));
        return *this;
    }

    ExitManagerBuilder& with_partial_take_profit(const std::vector<std::pair<double, double>>& targets) {
        manager_->add_take_profit(TakeProfitFactory::createPartial(targets));
        return *this;
    }

    // Time Exit ekle
    ExitManagerBuilder& with_max_hold_time(std::chrono::minutes duration) {
        manager_->add_time_exit(TimeExitFactory::createMaxHold(duration));
        return *this;
    }

    ExitManagerBuilder& with_session_exit(int hour, int minute = 0) {
        manager_->add_time_exit(TimeExitFactory::createSession(hour, minute));
        return *this;
    }

    ExitManagerBuilder& with_weekend_exit(int friday_hour = 21) {
        manager_->add_time_exit(TimeExitFactory::createWeekend(friday_hour));
        return *this;
    }

    ExitManagerBuilder& with_inactivity_exit(std::chrono::minutes timeout, double min_pips) {
        manager_->set_inactivity_exit(TimeExitFactory::createInactivity(timeout, min_pips));
        return *this;
    }

    // Jim Simons preset
    ExitManagerBuilder& with_jim_simons_preset(double sl_pips = 10.0) {
        // Fixed stop-loss
        manager_->add_stop_loss(StopLossFactory::createFixed(sl_pips));

        // 1.5R take-profit
        manager_->add_take_profit(TakeProfitFactory::createJimSimonsStyle(sl_pips));

        // Max 30 dakika pozisyon tutma
        manager_->add_time_exit(TimeExitFactory::createJimSimonsStyle());

        // 5 dakika hareketsizlikle cik
        manager_->set_inactivity_exit(
            std::make_unique<InactivityExit>(std::chrono::minutes(5), 2.0));

        // Trailing stop (5 pip karda aktive olsun, 3 pip izlesin)
        manager_->set_trailing_stop(
            std::make_unique<TrailingStopLoss>(3.0, 5.0));

        // Weekend exit
        manager_->add_time_exit(TimeExitFactory::createWeekend());

        return *this;
    }

    // Conservative preset
    ExitManagerBuilder& with_conservative_preset(double sl_pips = 15.0) {
        manager_->add_stop_loss(StopLossFactory::createFixed(sl_pips));
        manager_->add_take_profit(TakeProfitFactory::createRiskReward(2.0, sl_pips));
        manager_->add_time_exit(TimeExitFactory::createMaxHold(std::chrono::minutes(60)));
        manager_->add_time_exit(TimeExitFactory::createWeekend());
        return *this;
    }

    // Aggressive preset
    ExitManagerBuilder& with_aggressive_preset(double sl_pips = 8.0) {
        manager_->add_stop_loss(StopLossFactory::createFixed(sl_pips));
        manager_->add_take_profit(TakeProfitFactory::createRiskReward(1.2, sl_pips));
        manager_->add_time_exit(TimeExitFactory::createMaxHold(std::chrono::minutes(15)));
        manager_->set_trailing_stop(std::make_unique<TrailingStopLoss>(2.0, 3.0));
        manager_->add_time_exit(TimeExitFactory::createWeekend());
        return *this;
    }

    // Build
    std::unique_ptr<ExitManager> build() {
        return std::move(manager_);
    }

private:
    std::unique_ptr<ExitManager> manager_;
};

} // namespace gel::exit
