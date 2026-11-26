// include/gel/position/position.hpp
#pragma once

#include <chrono>
#include <string>
#include <optional>
#include <cmath>

namespace gel::position {

// Trade yonu
enum class Direction {
    LONG,   // BUY pozisyonu
    SHORT   // SELL pozisyonu
};

// Pozisyon durumu
enum class PositionStatus {
    OPEN,       // Acik pozisyon
    CLOSED,     // Kapatilmis
    PENDING     // Beklemede (henuz execute edilmemis)
};

// Pozisyon kapatma nedeni
enum class ExitReason {
    NONE,           // Henuz kapanmamis
    STOP_LOSS,      // Stop-loss tetiklendi
    TAKE_PROFIT,    // Take-profit tetiklendi
    TRAILING_STOP,  // Trailing stop tetiklendi
    TIME_EXIT,      // Zaman bazli cikis
    MANUAL,         // Manuel kapatma
    SESSION_CLOSE,  // Seans kapanisi
    MARGIN_CALL     // Margin call
};

// Pozisyon yapisi
struct Position {
    // Temel bilgiler
    std::string id;                 // Unique pozisyon ID
    std::string symbol;             // Parite (orn: "EURUSD")
    Direction direction;            // LONG veya SHORT
    PositionStatus status;          // Pozisyon durumu

    // Giris bilgileri
    double entry_price;             // Giris fiyati
    double size;                    // Pozisyon buyuklugu (lot)
    std::chrono::system_clock::time_point entry_time;  // Giris zamani

    // Cikis bilgileri
    std::optional<double> exit_price;   // Cikis fiyati
    std::optional<std::chrono::system_clock::time_point> exit_time;  // Cikis zamani
    ExitReason exit_reason = ExitReason::NONE;

    // Risk yonetimi
    std::optional<double> stop_loss;     // Stop-loss seviyesi
    std::optional<double> take_profit;   // Take-profit seviyesi
    std::optional<double> trailing_stop_distance;  // Trailing stop mesafesi (pip)

    // Izleme
    double highest_price = 0.0;     // Pozisyon acikken ulasilan en yuksek fiyat
    double lowest_price = 0.0;      // Pozisyon acikken ulasilan en dusuk fiyat
    double max_favorable_excursion = 0.0;   // En iyi noktadaki kar (pip)
    double max_adverse_excursion = 0.0;     // En kotu noktadaki zarar (pip)

    // Metadata (for tracking)
    double signal_confidence = 0.0;  // Signal confidence score
    std::string pattern_id;          // Pattern identifier (e.g., "FVG_BULL_123")
    double pip_value = 0.0001;        // Pip size for pair (default EUR/USD)

    // Constructors
    Position() = default;

    Position(const std::string& id,
             const std::string& symbol,
             Direction dir,
             double entry_price,
             double size,
             std::chrono::system_clock::time_point entry_time)
        : id(id)
        , symbol(symbol)
        , direction(dir)
        , status(PositionStatus::OPEN)
        , entry_price(entry_price)
        , size(size)
        , entry_time(entry_time)
        , highest_price(entry_price)
        , lowest_price(entry_price) {}

    // Pozisyonu guncelle (yeni fiyat geldiginde)
    void update_price(double current_price) {
        if (status != PositionStatus::OPEN) return;

        // Tracking icin en yuksek/dusuk fiyatlari guncelle
        if (current_price > highest_price) {
            highest_price = current_price;
        }
        if (current_price < lowest_price) {
            lowest_price = current_price;
        }

        // MFE ve MAE hesapla
        double unrealized_pnl = unrealized_pnl_pips(current_price);
        if (unrealized_pnl > max_favorable_excursion) {
            max_favorable_excursion = unrealized_pnl;
        }
        if (unrealized_pnl < -max_adverse_excursion) {
            max_adverse_excursion = -unrealized_pnl;
        }
    }

    // Gerceklesmemis kar/zarar (pip cinsinden)
    [[nodiscard]] double unrealized_pnl_pips(double current_price) const {
        if (status != PositionStatus::OPEN) return 0.0;

        double pips = 0.0;
        if (direction == Direction::LONG) {
            pips = (current_price - entry_price) * pip_multiplier();
        } else {
            pips = (entry_price - current_price) * pip_multiplier();
        }
        return pips;
    }

    // Gerceklesmemis kar/zarar (dolar cinsinden)
    [[nodiscard]] double unrealized_pnl_dollars(double current_price, double pip_value = 10.0) const {
        return unrealized_pnl_pips(current_price) * size * pip_value;
    }

    // Gerceklesmis kar/zarar (pip cinsinden) - sadece kapanmis pozisyonlar icin
    [[nodiscard]] double realized_pnl_pips() const {
        if (status != PositionStatus::CLOSED || !exit_price.has_value()) return 0.0;

        double pips = 0.0;
        if (direction == Direction::LONG) {
            pips = (exit_price.value() - entry_price) * pip_multiplier();
        } else {
            pips = (entry_price - exit_price.value()) * pip_multiplier();
        }
        return pips;
    }

    // Gerceklesmis kar/zarar (dolar cinsinden)
    [[nodiscard]] double realized_pnl_dollars(double pip_value = 10.0) const {
        return realized_pnl_pips() * size * pip_value;
    }

    // Helper: Is position long?
    [[nodiscard]] bool is_long() const {
        return direction == Direction::LONG;
    }

    // Pozisyon suresi
    [[nodiscard]] std::chrono::seconds duration(std::chrono::system_clock::time_point current_time) const {
        if (status == PositionStatus::CLOSED && exit_time.has_value()) {
            return std::chrono::duration_cast<std::chrono::seconds>(exit_time.value() - entry_time);
        }
        return std::chrono::duration_cast<std::chrono::seconds>(current_time - entry_time);
    }

    // Pozisyonu kapat
    void close(double price,
               std::chrono::system_clock::time_point time,
               ExitReason reason) {
        exit_price = price;
        exit_time = time;
        exit_reason = reason;
        status = PositionStatus::CLOSED;
    }

    // Stop-loss tetiklendi mi?
    [[nodiscard]] bool is_stop_loss_hit(double current_price) const {
        if (!stop_loss.has_value() || status != PositionStatus::OPEN) return false;

        if (direction == Direction::LONG) {
            return current_price <= stop_loss.value();
        } else {
            return current_price >= stop_loss.value();
        }
    }

    // Take-profit tetiklendi mi?
    [[nodiscard]] bool is_take_profit_hit(double current_price) const {
        if (!take_profit.has_value() || status != PositionStatus::OPEN) return false;

        if (direction == Direction::LONG) {
            return current_price >= take_profit.value();
        } else {
            return current_price <= take_profit.value();
        }
    }

    // Pip carpani (major forex ciftleri icin genellikle 10000)
    [[nodiscard]] double pip_multiplier() const {
        // JPY ciftleri icin 100, digerleri icin 10000
        if (symbol.find("JPY") != std::string::npos) {
            return 100.0;
        }
        return 10000.0;
    }

    // Pip degeri hesapla (1 pip = ? birim)
    [[nodiscard]] double pip_size() const {
        if (symbol.find("JPY") != std::string::npos) {
            return 0.01;
        }
        return 0.0001;
    }
};

// Pozisyon istatistikleri
struct PositionStats {
    int total_trades = 0;
    int winning_trades = 0;
    int losing_trades = 0;

    double total_pips = 0.0;
    double total_profit = 0.0;
    double total_loss = 0.0;

    double max_win = 0.0;
    double max_loss = 0.0;

    int max_consecutive_wins = 0;
    int max_consecutive_losses = 0;
    int current_consecutive_wins = 0;
    int current_consecutive_losses = 0;

    // Win rate hesapla
    [[nodiscard]] double win_rate() const {
        if (total_trades == 0) return 0.0;
        return static_cast<double>(winning_trades) / total_trades * 100.0;
    }

    // Profit factor hesapla
    [[nodiscard]] double profit_factor() const {
        if (std::abs(total_loss) < 0.0001) return 0.0;
        return std::abs(total_profit) / std::abs(total_loss);
    }

    // Ortalama kazanc
    [[nodiscard]] double average_win() const {
        if (winning_trades == 0) return 0.0;
        return total_profit / winning_trades;
    }

    // Ortalama kayip
    [[nodiscard]] double average_loss() const {
        if (losing_trades == 0) return 0.0;
        return total_loss / losing_trades;
    }

    // Expectancy (R-multiple)
    [[nodiscard]] double expectancy() const {
        if (total_trades == 0) return 0.0;
        double wr = win_rate() / 100.0;
        double lr = 1.0 - wr;
        return (wr * average_win()) - (lr * std::abs(average_loss()));
    }

    // Static method: Calculate stats from position list
    static PositionStats calculate(const std::vector<Position>& positions) {
        PositionStats stats;
        for (const auto& pos : positions) {
            stats.add_position(pos);
        }
        return stats;
    }

    // Pozisyon ekle ve istatistikleri guncelle
    void add_position(const Position& pos) {
        if (pos.status != PositionStatus::CLOSED) return;

        total_trades++;
        double pnl = pos.realized_pnl_pips();
        total_pips += pnl;

        if (pnl > 0) {
            winning_trades++;
            total_profit += pnl;
            if (pnl > max_win) max_win = pnl;

            current_consecutive_wins++;
            current_consecutive_losses = 0;
            if (current_consecutive_wins > max_consecutive_wins) {
                max_consecutive_wins = current_consecutive_wins;
            }
        } else if (pnl < 0) {
            losing_trades++;
            total_loss += pnl;
            if (pnl < max_loss) max_loss = pnl;

            current_consecutive_losses++;
            current_consecutive_wins = 0;
            if (current_consecutive_losses > max_consecutive_losses) {
                max_consecutive_losses = current_consecutive_losses;
            }
        }
    }
};

} // namespace gel::position
