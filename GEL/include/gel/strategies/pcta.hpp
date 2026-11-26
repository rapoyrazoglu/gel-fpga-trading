// include/gel/strategies/pcta.hpp
//
// PCTA — Proactive Calendar-driven Trading Agent.
// Owner: Deniz. Python PPO + LLM hybrid; this file is the C++ bridge that
// makes PCTA look like any other IStrategy to GEL.
//
// Two integration modes:
//
//   1. SHM bridge (production)
//        Python policy writes Orders into a SHM ring. PCTAStrategy reads
//        from that ring on each on_tick() and forwards them via the GEL
//        OrderSink. on_event() and on_fill() are pushed back to Python.
//
//   2. Direct callback (testing)
//        A C++ "decision callback" is invoked on each event. Used in
//        unit tests and the integrated_demo so we don't need Python at
//        defense time.
//
// The dividend-window logic from deck slide 10 lives in Python on the
// PPO side; the C++ side only translates Order/event messages.
//
#pragma once

#include "connector/strategy.hpp"
#include <atomic>
#include <functional>
#include <unordered_map>

namespace gel::strategies {

    using connector::IStrategy;
    using connector::OrderSink;
    using connector::Order;
    using connector::Tick;
    using connector::CalendarEvent;
    using connector::ExecReport;
    using connector::StrategyId;
    using connector::EventKind;

    struct PctaConfig {
        // Pre-window length (deck: -14d entry .. -1d entry).
        int  pre_window_days  = 14;
        int  post_window_days = 10;
        // Position sizing — capped at 2% of next-day liquidity (deck slide 10).
        double max_pct_daily_volume = 0.02;
        // Risk envelope.
        int64_t max_abs_position = 1'000'000;
    };

    // Pluggable decision policy — swap PPO+LLM (Python) for a stub in tests.
    class IPctaPolicy {
    public:
        virtual ~IPctaPolicy() = default;
        // Called on every CalendarEvent. May emit any number of orders
        // by calling sink.send().
        virtual void on_event(const CalendarEvent& e, OrderSink& sink) = 0;
        // Called on every tick — typically for closing logic.
        virtual void on_tick (const Tick& t, OrderSink& sink) { (void)t; (void)sink; }
        // Reward signal back to PPO.
        virtual void on_fill (const ExecReport& r) { (void)r; }
    };

    class PCTAStrategy final : public IStrategy {
    public:
        PCTAStrategy(PctaConfig cfg, std::unique_ptr<IPctaPolicy> policy)
            : cfg_(cfg), policy_(std::move(policy)) {}

        StrategyId  id()   const noexcept override { return StrategyId::PCTA; }
        std::string name() const override          { return "PCTA"; }

        void on_start(OrderSink& sink) override { sink_ = &sink; }
        void on_stop() override                 { sink_ = nullptr; }

        void on_tick(const Tick& t) override {
            if (sink_ && policy_) policy_->on_tick(t, *sink_);
        }
        void on_event(const CalendarEvent& e) override {
            if (sink_ && policy_) policy_->on_event(e, *sink_);
        }
        void on_fill(const ExecReport& r) override {
            if (policy_) policy_->on_fill(r);
            ++fills_;
        }
        void on_kill_switch(connector::RejectReason) override { active_ = false; }
        void on_resume() override                            { active_ = true;  }

        const PctaConfig& config() const { return cfg_; }
        uint64_t fills() const           { return fills_.load(); }

    private:
        PctaConfig                    cfg_;
        std::unique_ptr<IPctaPolicy>  policy_;
        OrderSink*                    sink_   = nullptr;
        std::atomic<bool>             active_{true};
        std::atomic<uint64_t>         fills_{0};
    };

} // namespace gel::strategies
