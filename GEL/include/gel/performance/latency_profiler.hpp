// include/gel/performance/latency_profiler.hpp
#pragma once

#include <chrono>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <optional>

namespace gel::performance {

    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;
    using Duration = std::chrono::nanoseconds;

    /**
     * LatencyMeasurement - Single latency measurement with context
     */
    struct LatencyMeasurement {
        Duration tick_to_indicator;      // Tick -> Indicator calculation
        Duration indicator_to_signal;    // Indicator -> Signal generation
        Duration signal_to_decision;     // Signal -> Trading decision
        Duration total_tick_to_signal;   // Total pipeline latency

        TimePoint timestamp;             // When measurement was taken
        std::string tick_id;             // Identifier for the tick
        bool signal_generated;           // Whether a signal was produced
        double signal_pnl;               // P&L if signal was executed (optional)

        [[nodiscard]] Duration total() const {
            return tick_to_indicator + indicator_to_signal + signal_to_decision;
        }
    };

    /**
     * LatencyStatistics - Aggregated latency statistics
     */
    struct LatencyStatistics {
        size_t sample_count = 0;

        // Nanosecond measurements
        int64_t min_ns = 0;
        int64_t max_ns = 0;
        double mean_ns = 0.0;
        double std_dev_ns = 0.0;

        // Percentiles
        int64_t p50_ns = 0;   // Median
        int64_t p90_ns = 0;   // 90th percentile
        int64_t p95_ns = 0;   // 95th percentile
        int64_t p99_ns = 0;   // 99th percentile

        // Microsecond helpers
        [[nodiscard]] double min_us() const { return min_ns / 1000.0; }
        [[nodiscard]] double max_us() const { return max_ns / 1000.0; }
        [[nodiscard]] double mean_us() const { return mean_ns / 1000.0; }
        [[nodiscard]] double p50_us() const { return p50_ns / 1000.0; }
        [[nodiscard]] double p90_us() const { return p90_ns / 1000.0; }
        [[nodiscard]] double p95_us() const { return p95_ns / 1000.0; }
        [[nodiscard]] double p99_us() const { return p99_ns / 1000.0; }

        // Millisecond helpers
        [[nodiscard]] double min_ms() const { return min_ns / 1000000.0; }
        [[nodiscard]] double max_ms() const { return max_ns / 1000000.0; }
        [[nodiscard]] double mean_ms() const { return mean_ns / 1000000.0; }

        [[nodiscard]] std::string to_string() const {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2);
            oss << "Latency Statistics (n=" << sample_count << "):\n";
            oss << "  Min:    " << min_us() << " µs\n";
            oss << "  Max:    " << max_us() << " µs\n";
            oss << "  Mean:   " << mean_us() << " µs\n";
            oss << "  StdDev: " << (std_dev_ns / 1000.0) << " µs\n";
            oss << "  P50:    " << p50_us() << " µs\n";
            oss << "  P90:    " << p90_us() << " µs\n";
            oss << "  P95:    " << p95_us() << " µs\n";
            oss << "  P99:    " << p99_us() << " µs\n";
            return oss.str();
        }
    };

    /**
     * LatencyPnLCorrelation - Correlation between latency and P&L
     */
    struct LatencyPnLCorrelation {
        double correlation_coefficient = 0.0;  // Pearson correlation
        double avg_latency_winners_ns = 0.0;   // Avg latency for winning trades
        double avg_latency_losers_ns = 0.0;    // Avg latency for losing trades
        size_t winning_trades = 0;
        size_t losing_trades = 0;

        [[nodiscard]] std::string to_string() const {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(4);
            oss << "Latency-P&L Correlation:\n";
            oss << "  Correlation Coefficient: " << correlation_coefficient << "\n";
            oss << "  Avg Latency (Winners):   " << (avg_latency_winners_ns / 1000.0) << " µs\n";
            oss << "  Avg Latency (Losers):    " << (avg_latency_losers_ns / 1000.0) << " µs\n";
            oss << "  Winning Trades:          " << winning_trades << "\n";
            oss << "  Losing Trades:           " << losing_trades << "\n";
            return oss.str();
        }
    };

    /**
     * ScopedTimer - RAII timer for measuring code blocks
     */
    class ScopedTimer {
    public:
        explicit ScopedTimer(Duration& output)
            : output_(output), start_(Clock::now()) {}

        ~ScopedTimer() {
            output_ = Clock::now() - start_;
        }

        // Non-copyable
        ScopedTimer(const ScopedTimer&) = delete;
        ScopedTimer& operator=(const ScopedTimer&) = delete;

    private:
        Duration& output_;
        TimePoint start_;
    };

    /**
     * LatencyProfiler - Main profiling class for tick-to-signal pipeline
     *
     * Usage:
     *   LatencyProfiler profiler;
     *
     *   // Start measurement
     *   profiler.start_tick("tick_123");
     *
     *   // After indicator calculation
     *   profiler.mark_indicator_done();
     *
     *   // After signal generation
     *   profiler.mark_signal_done(signal_generated);
     *
     *   // After trading decision
     *   profiler.end_tick(pnl);  // pnl is optional
     *
     *   // Get statistics
     *   auto stats = profiler.get_statistics();
     */
    class LatencyProfiler {
    public:
        LatencyProfiler() = default;

        /**
         * Start timing a new tick
         */
        void start_tick(const std::string& tick_id = "") {
            std::lock_guard<std::mutex> lock(mutex_);
            current_.tick_id = tick_id;
            current_.timestamp = Clock::now();
            tick_start_ = Clock::now();
            indicator_done_ = std::nullopt;
            signal_done_ = std::nullopt;
        }

        /**
         * Mark indicator calculation as done
         */
        void mark_indicator_done() {
            std::lock_guard<std::mutex> lock(mutex_);
            indicator_done_ = Clock::now();
            if (tick_start_.time_since_epoch().count() > 0) {
                current_.tick_to_indicator = *indicator_done_ - tick_start_;
            }
        }

        /**
         * Mark signal generation as done
         */
        void mark_signal_done(bool signal_generated = false) {
            std::lock_guard<std::mutex> lock(mutex_);
            signal_done_ = Clock::now();
            current_.signal_generated = signal_generated;

            if (indicator_done_.has_value()) {
                current_.indicator_to_signal = *signal_done_ - *indicator_done_;
            }
        }

        /**
         * End tick timing and record measurement
         */
        void end_tick(double pnl = 0.0) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto now = Clock::now();

            if (signal_done_.has_value()) {
                current_.signal_to_decision = now - *signal_done_;
            }

            current_.total_tick_to_signal = now - tick_start_;
            current_.signal_pnl = pnl;

            measurements_.push_back(current_);

            // Reset for next tick
            current_ = LatencyMeasurement{};
        }

        /**
         * Quick measurement - measures total tick-to-signal time only
         */
        void record_quick_measurement(Duration total_latency, bool signal_generated = false, double pnl = 0.0) {
            std::lock_guard<std::mutex> lock(mutex_);
            LatencyMeasurement m;
            m.timestamp = Clock::now();
            m.total_tick_to_signal = total_latency;
            m.signal_generated = signal_generated;
            m.signal_pnl = pnl;
            measurements_.push_back(m);
        }

        /**
         * Get statistics for total tick-to-signal latency
         */
        [[nodiscard]] LatencyStatistics get_statistics() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return calculate_statistics([](const LatencyMeasurement& m) {
                return m.total_tick_to_signal.count();
            });
        }

        /**
         * Get statistics for tick-to-indicator latency
         */
        [[nodiscard]] LatencyStatistics get_tick_to_indicator_stats() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return calculate_statistics([](const LatencyMeasurement& m) {
                return m.tick_to_indicator.count();
            });
        }

        /**
         * Get statistics for indicator-to-signal latency
         */
        [[nodiscard]] LatencyStatistics get_indicator_to_signal_stats() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return calculate_statistics([](const LatencyMeasurement& m) {
                return m.indicator_to_signal.count();
            });
        }

        /**
         * Get statistics for signal-to-decision latency
         */
        [[nodiscard]] LatencyStatistics get_signal_to_decision_stats() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return calculate_statistics([](const LatencyMeasurement& m) {
                return m.signal_to_decision.count();
            });
        }

        /**
         * Calculate correlation between latency and P&L
         */
        [[nodiscard]] LatencyPnLCorrelation get_latency_pnl_correlation() const {
            std::lock_guard<std::mutex> lock(mutex_);

            LatencyPnLCorrelation result;

            // Filter measurements with signals and P&L
            std::vector<double> latencies;
            std::vector<double> pnls;
            double sum_latency_winners = 0.0;
            double sum_latency_losers = 0.0;

            for (const auto& m : measurements_) {
                if (m.signal_generated && m.signal_pnl != 0.0) {
                    double lat = static_cast<double>(m.total_tick_to_signal.count());
                    latencies.push_back(lat);
                    pnls.push_back(m.signal_pnl);

                    if (m.signal_pnl > 0) {
                        result.winning_trades++;
                        sum_latency_winners += lat;
                    } else {
                        result.losing_trades++;
                        sum_latency_losers += lat;
                    }
                }
            }

            if (latencies.size() < 2) {
                return result;
            }

            // Calculate averages
            if (result.winning_trades > 0) {
                result.avg_latency_winners_ns = sum_latency_winners / result.winning_trades;
            }
            if (result.losing_trades > 0) {
                result.avg_latency_losers_ns = sum_latency_losers / result.losing_trades;
            }

            // Calculate Pearson correlation coefficient
            double mean_lat = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
            double mean_pnl = std::accumulate(pnls.begin(), pnls.end(), 0.0) / pnls.size();

            double numerator = 0.0;
            double sum_sq_lat = 0.0;
            double sum_sq_pnl = 0.0;

            for (size_t i = 0; i < latencies.size(); ++i) {
                double diff_lat = latencies[i] - mean_lat;
                double diff_pnl = pnls[i] - mean_pnl;
                numerator += diff_lat * diff_pnl;
                sum_sq_lat += diff_lat * diff_lat;
                sum_sq_pnl += diff_pnl * diff_pnl;
            }

            double denominator = std::sqrt(sum_sq_lat * sum_sq_pnl);
            if (denominator > 0) {
                result.correlation_coefficient = numerator / denominator;
            }

            return result;
        }

        /**
         * Get all measurements
         */
        [[nodiscard]] std::vector<LatencyMeasurement> get_measurements() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return measurements_;
        }

        /**
         * Get number of measurements
         */
        [[nodiscard]] size_t count() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return measurements_.size();
        }

        /**
         * Clear all measurements
         */
        void clear() {
            std::lock_guard<std::mutex> lock(mutex_);
            measurements_.clear();
            current_ = LatencyMeasurement{};
        }

        /**
         * Get throughput (ticks per second)
         */
        [[nodiscard]] double get_throughput() const {
            std::lock_guard<std::mutex> lock(mutex_);
            if (measurements_.size() < 2) return 0.0;

            auto first = measurements_.front().timestamp;
            auto last = measurements_.back().timestamp;
            auto duration = std::chrono::duration<double>(last - first);

            if (duration.count() <= 0) return 0.0;
            return measurements_.size() / duration.count();
        }

        /**
         * Generate comprehensive report
         */
        [[nodiscard]] std::string generate_report() const {
            std::ostringstream oss;
            oss << "============================================\n";
            oss << "       LATENCY PROFILER REPORT\n";
            oss << "============================================\n\n";

            oss << "Total Measurements: " << count() << "\n";
            oss << "Throughput: " << std::fixed << std::setprecision(2)
                << get_throughput() << " ticks/sec\n\n";

            oss << "--- Total Tick-to-Signal ---\n";
            oss << get_statistics().to_string() << "\n";

            oss << "--- Tick-to-Indicator ---\n";
            oss << get_tick_to_indicator_stats().to_string() << "\n";

            oss << "--- Indicator-to-Signal ---\n";
            oss << get_indicator_to_signal_stats().to_string() << "\n";

            oss << "--- Signal-to-Decision ---\n";
            oss << get_signal_to_decision_stats().to_string() << "\n";

            oss << "--- Latency vs P&L ---\n";
            oss << get_latency_pnl_correlation().to_string() << "\n";

            oss << "============================================\n";

            return oss.str();
        }

    private:
        mutable std::mutex mutex_;
        std::vector<LatencyMeasurement> measurements_;

        // Current measurement state
        LatencyMeasurement current_;
        TimePoint tick_start_;
        std::optional<TimePoint> indicator_done_;
        std::optional<TimePoint> signal_done_;

        /**
         * Calculate statistics from measurements using a value extractor
         */
        template<typename Extractor>
        LatencyStatistics calculate_statistics(Extractor extractor) const {
            LatencyStatistics stats;

            if (measurements_.empty()) {
                return stats;
            }

            // Extract values
            std::vector<int64_t> values;
            values.reserve(measurements_.size());
            for (const auto& m : measurements_) {
                values.push_back(extractor(m));
            }

            stats.sample_count = values.size();

            // Sort for percentiles
            std::vector<int64_t> sorted = values;
            std::sort(sorted.begin(), sorted.end());

            // Min/Max
            stats.min_ns = sorted.front();
            stats.max_ns = sorted.back();

            // Mean
            double sum = std::accumulate(values.begin(), values.end(), 0.0);
            stats.mean_ns = sum / values.size();

            // Standard deviation
            double sq_sum = 0.0;
            for (auto v : values) {
                double diff = v - stats.mean_ns;
                sq_sum += diff * diff;
            }
            stats.std_dev_ns = std::sqrt(sq_sum / values.size());

            // Percentiles
            auto percentile = [&sorted](double p) -> int64_t {
                size_t idx = static_cast<size_t>(p * (sorted.size() - 1));
                return sorted[idx];
            };

            stats.p50_ns = percentile(0.50);
            stats.p90_ns = percentile(0.90);
            stats.p95_ns = percentile(0.95);
            stats.p99_ns = percentile(0.99);

            return stats;
        }
    };

    /**
     * PipelineProfiler - Profiles the entire trading pipeline
     *
     * Measures:
     * - Tick ingestion
     * - OHLC aggregation
     * - Indicator calculation
     * - Pattern detection
     * - Signal generation
     * - Order execution
     */
    class PipelineProfiler {
    public:
        PipelineProfiler() = default;

        // Stage timers
        LatencyProfiler tick_ingestion;
        LatencyProfiler ohlc_aggregation;
        LatencyProfiler indicator_calc;
        LatencyProfiler pattern_detection;
        LatencyProfiler signal_generation;
        LatencyProfiler order_execution;

        // Overall pipeline
        LatencyProfiler total_pipeline;

        /**
         * Generate full pipeline report
         */
        [[nodiscard]] std::string generate_full_report() const {
            std::ostringstream oss;
            oss << "============================================\n";
            oss << "    FULL PIPELINE PROFILER REPORT\n";
            oss << "============================================\n\n";

            auto report_stage = [&oss](const std::string& name, const LatencyProfiler& profiler) {
                if (profiler.count() > 0) {
                    oss << "--- " << name << " ---\n";
                    auto stats = profiler.get_statistics();
                    oss << "  Samples: " << stats.sample_count << "\n";
                    oss << "  Mean:    " << std::fixed << std::setprecision(2) << stats.mean_us() << " µs\n";
                    oss << "  P99:     " << stats.p99_us() << " µs\n\n";
                }
            };

            report_stage("Tick Ingestion", tick_ingestion);
            report_stage("OHLC Aggregation", ohlc_aggregation);
            report_stage("Indicator Calculation", indicator_calc);
            report_stage("Pattern Detection", pattern_detection);
            report_stage("Signal Generation", signal_generation);
            report_stage("Order Execution", order_execution);
            report_stage("Total Pipeline", total_pipeline);

            oss << "============================================\n";

            return oss.str();
        }

        void clear_all() {
            tick_ingestion.clear();
            ohlc_aggregation.clear();
            indicator_calc.clear();
            pattern_detection.clear();
            signal_generation.clear();
            order_execution.clear();
            total_pipeline.clear();
        }
    };

} // namespace gel::performance
