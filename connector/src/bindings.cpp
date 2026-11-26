// src/bindings.cpp
//
// pybind11 bindings — exposes the connector SDK to Python.
//
// What's here (the strategy-author surface):
//   - All wire types (Order, Tick, BookUpdate, CalendarEvent, ExecReport)
//   - All enums (StrategyId, Side, OrdType, TIF, EventKind, EventImpact,
//                ExecStatus, RejectReason)
//   - PyStrategy with a trampoline so a Python class can subclass it
//   - ShmClient for cross-process strategy → engine IPC
//   - AccountInfo + StrategyAccountMap
//   - to_ticks / from_ticks helpers
//
// What's NOT here:
//   - Engine. That belongs to the engine repo (GEL). Engine ties together
//     risk + router + drop-copy, which are platform code. The connector
//     stays free of those dependencies so a research strategy in Python
//     can be built and tested without compiling GEL.
//
// Usage from Python:
//
//     import connector as c
//
//     class MyPolicy(c.PyStrategy):
//         def __init__(self):
//             super().__init__(strategy_id=int(c.StrategyId.PCTA), name="PCTA")
//         def on_event(self, e):
//             if e.impact == int(c.EventImpact.HIGH):
//                 self.send_order(c.Order.market(sid=1, qty=1000, side=c.Side.BUY))
//
//     # Cross-process: connect to the GEL daemon's SHM rings.
//     client = c.ShmClient("/tmp/orders.shm", "/tmp/reports.shm")
//     client.send_order(c.Order.market(sid=1, qty=100, side=c.Side.BUY))
//
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "connector/account_map.hpp"
#include "connector/shm_client.hpp"
#include "connector/sinks.hpp"
#include "connector/strategy.hpp"
#include "connector/wire.hpp"

namespace py = pybind11;
using namespace connector;

// ----------------------------------------------------------------------
// PyStrategy — concrete IStrategy that holds id/name/sink. This is the
// class Python users subclass. Its virtual methods default to no-ops; the
// PyStrategyTramp trampoline forwards C++ → Python overrides.
// ----------------------------------------------------------------------
class PyStrategy : public IStrategy {
public:
    PyStrategy(uint32_t sid_int, std::string nm)
        : sid_(static_cast<StrategyId>(sid_int)), name_(std::move(nm)) {}

    StrategyId  id()   const noexcept override { return sid_; }
    std::string name() const override          { return name_; }

    void on_start(OrderSink& sink) override { sink_ = &sink; }

    // Helper exposed to Python: send_order(order). Returns False if no
    // sink is wired up or the ring is full.
    bool send_order(const Order& o) {
        return sink_ ? sink_->send(o) : false;
    }

private:
    StrategyId  sid_;
    std::string name_;
    OrderSink*  sink_ = nullptr;
};

class PyStrategyTramp : public PyStrategy {
public:
    using PyStrategy::PyStrategy;

    void on_start(OrderSink& s) override {
        PyStrategy::on_start(s);
        PYBIND11_OVERRIDE(void, PyStrategy, on_start, std::ref(s));
    }
    void on_stop()                        override { PYBIND11_OVERRIDE(void, PyStrategy, on_stop); }
    void on_tick (const Tick& t)          override { PYBIND11_OVERRIDE(void, PyStrategy, on_tick,  t); }
    void on_book (const BookUpdate& b)    override { PYBIND11_OVERRIDE(void, PyStrategy, on_book,  b); }
    void on_event(const CalendarEvent& e) override { PYBIND11_OVERRIDE(void, PyStrategy, on_event, e); }
    void on_fill (const ExecReport& r)    override { PYBIND11_OVERRIDE(void, PyStrategy, on_fill,  r); }
    void on_kill_switch(RejectReason r)   override { PYBIND11_OVERRIDE(void, PyStrategy, on_kill_switch, r); }
    void on_resume()                      override { PYBIND11_OVERRIDE(void, PyStrategy, on_resume); }
};

// ----------------------------------------------------------------------
// Module
// ----------------------------------------------------------------------
PYBIND11_MODULE(_connector_native, m) {
    m.doc() = "connector — strategy ↔ engine SDK (Python bindings)";

    // -----------------------------------------------------------------
    // Enums
    // -----------------------------------------------------------------
    py::enum_<StrategyId>(m, "StrategyId")
        .value("UNKNOWN",      StrategyId::UNKNOWN)
        .value("PCTA",         StrategyId::PCTA)
        .value("MARKET_MAKER", StrategyId::MARKET_MAKER);

    py::enum_<Side>(m, "Side")
        .value("BUY",  Side::BUY)
        .value("SELL", Side::SELL);

    py::enum_<OrdType>(m, "OrdType")
        .value("MARKET",    OrdType::MARKET)
        .value("LIMIT",     OrdType::LIMIT)
        .value("IOC",       OrdType::IOC)
        .value("FOK",       OrdType::FOK)
        .value("POST_ONLY", OrdType::POST_ONLY)
        .value("CANCEL",    OrdType::CANCEL)
        .value("REPLACE",   OrdType::REPLACE);

    py::enum_<TIF>(m, "TIF")
        .value("DAY", TIF::DAY)
        .value("IOC", TIF::IOC)
        .value("FOK", TIF::FOK)
        .value("GTC", TIF::GTC);

    py::enum_<EventKind>(m, "EventKind")
        .value("UNKNOWN",  EventKind::UNKNOWN)
        .value("DIVIDEND", EventKind::DIVIDEND)
        .value("FOMC",     EventKind::FOMC)
        .value("NFP",      EventKind::NFP)
        .value("CPI",      EventKind::CPI)
        .value("ECB",      EventKind::ECB)
        .value("EARNINGS", EventKind::EARNINGS);

    py::enum_<EventImpact>(m, "EventImpact")
        .value("LOW",    EventImpact::LOW)
        .value("MEDIUM", EventImpact::MEDIUM)
        .value("HIGH",   EventImpact::HIGH);

    py::enum_<ExecStatus>(m, "ExecStatus")
        .value("ACCEPTED", ExecStatus::ACCEPTED)
        .value("REJECTED", ExecStatus::REJECTED)
        .value("PARTIAL",  ExecStatus::PARTIAL)
        .value("FILLED",   ExecStatus::FILLED)
        .value("CANCELED", ExecStatus::CANCELED)
        .value("EXPIRED",  ExecStatus::EXPIRED);

    py::enum_<RejectReason>(m, "RejectReason")
        .value("NONE",           RejectReason::NONE)
        .value("FAT_FINGER",     RejectReason::FAT_FINGER)
        .value("POSITION_LIMIT", RejectReason::POSITION_LIMIT)
        .value("THROTTLE",       RejectReason::THROTTLE)
        .value("UNKNOWN_SYMBOL", RejectReason::UNKNOWN_SYMBOL)
        .value("KILL_SWITCH",    RejectReason::KILL_SWITCH)
        .value("BAD_PRICE",      RejectReason::BAD_PRICE)
        .value("BAD_QTY",        RejectReason::BAD_QTY)
        .value("STALE_ORDER",    RejectReason::STALE_ORDER);

    // -----------------------------------------------------------------
    // Wire types
    // -----------------------------------------------------------------
    py::class_<Order>(m, "Order")
        .def(py::init<>())
        .def_readwrite("ts_ns",           &Order::ts_ns)
        .def_readwrite("client_order_id", &Order::client_order_id)
        .def_readwrite("strategy_id",     &Order::strategy_id)
        .def_readwrite("symbol_id",       &Order::symbol_id)
        .def_readwrite("price_ticks",     &Order::price_ticks)
        .def_readwrite("quantity",        &Order::quantity)
        .def_readwrite("side",            &Order::side)
        .def_readwrite("ord_type",        &Order::ord_type)
        .def_readwrite("tif",             &Order::tif)
        .def_readwrite("flags",           &Order::flags)
        .def_readwrite("risk_budget_bps", &Order::risk_budget_bps)
        .def_readwrite("account_id",      &Order::account_id)
        .def_readwrite("ref_order_id",    &Order::ref_order_id)
        .def_static("market", [](SymbolId sid, int64_t qty, Side side) {
            Order o{};
            o.symbol_id   = sid;
            o.quantity    = qty;
            o.side        = static_cast<uint8_t>(side);
            o.ord_type    = static_cast<uint8_t>(OrdType::MARKET);
            o.tif         = static_cast<uint8_t>(TIF::DAY);
            o.price_ticks = -1;
            return o;
        }, py::arg("sid"), py::arg("qty"), py::arg("side"));

    py::class_<Tick>(m, "Tick")
        .def(py::init<>())
        .def_readwrite("ts_ns",     &Tick::ts_ns)
        .def_readwrite("symbol_id", &Tick::symbol_id)
        .def_readwrite("seq",       &Tick::seq)
        .def_readwrite("bid_ticks", &Tick::bid_ticks)
        .def_readwrite("ask_ticks", &Tick::ask_ticks);

    py::class_<CalendarEvent>(m, "CalendarEvent")
        .def(py::init<>())
        .def_readwrite("event_ts_ns",    &CalendarEvent::event_ts_ns)
        .def_readwrite("ingest_ts_ns",   &CalendarEvent::ingest_ts_ns)
        .def_readwrite("symbol_id",      &CalendarEvent::symbol_id)
        .def_readwrite("kind",           &CalendarEvent::kind)
        .def_readwrite("impact",         &CalendarEvent::impact)
        .def_readwrite("consensus_x1e6", &CalendarEvent::consensus_x1e6)
        .def_readwrite("actual_x1e6",    &CalendarEvent::actual_x1e6)
        .def_readwrite("previous_x1e6",  &CalendarEvent::previous_x1e6);

    py::class_<ExecReport>(m, "ExecReport")
        .def(py::init<>())
        .def_readonly("ts_ns",             &ExecReport::ts_ns)
        .def_readonly("client_order_id",   &ExecReport::client_order_id)
        .def_readonly("exchange_order_id", &ExecReport::exchange_order_id)
        .def_readonly("strategy_id",       &ExecReport::strategy_id)
        .def_readonly("symbol_id",         &ExecReport::symbol_id)
        .def_readonly("fill_price_ticks",  &ExecReport::fill_price_ticks)
        .def_readonly("fill_quantity",     &ExecReport::fill_quantity)
        .def_readonly("status",            &ExecReport::status)
        .def_readonly("reject_reason",     &ExecReport::reject_reason)
        .def_readonly("side",              &ExecReport::side)
        .def_readonly("account_id",        &ExecReport::account_id);

    // -----------------------------------------------------------------
    // Strategy
    // -----------------------------------------------------------------
    py::class_<PyStrategy, PyStrategyTramp, std::shared_ptr<PyStrategy>>(m, "PyStrategy")
        .def(py::init<uint32_t, std::string>(),
             py::arg("strategy_id"), py::arg("name"))
        .def("send_order",     &PyStrategy::send_order, py::arg("order"))
        .def("on_start",       &PyStrategy::on_start,   py::arg("sink"))
        .def("on_stop",        &IStrategy::on_stop)
        .def("on_tick",        &IStrategy::on_tick,    py::arg("t"))
        .def("on_book",        &IStrategy::on_book,    py::arg("b"))
        .def("on_event",       &IStrategy::on_event,   py::arg("e"))
        .def("on_fill",        &IStrategy::on_fill,    py::arg("r"))
        .def("on_kill_switch", &IStrategy::on_kill_switch, py::arg("reason"))
        .def("on_resume",      &IStrategy::on_resume);

    // -----------------------------------------------------------------
    // Cross-process IPC
    // -----------------------------------------------------------------
    py::class_<ShmClient>(m, "ShmClient",
        "Cross-process strategy-side bridge. Attaches to a pair of mmap'd "
        "ring files the engine has already created.")
        .def(py::init<const std::string&, const std::string&>(),
             py::arg("orders_path"), py::arg("reports_path"))
        .def("send_order",      &ShmClient::send_order,     py::arg("order"))
        .def("poll_report",
             [](ShmClient& self) -> py::object {
                 ExecReport r{};
                 if (self.poll_report(r)) return py::cast(r);
                 return py::none();
             },
             "Pop the next ExecReport, or None if the ring is empty.")
        .def_property_readonly("orders_pending",  &ShmClient::orders_pending)
        .def_property_readonly("reports_pending", &ShmClient::reports_pending);

    // -----------------------------------------------------------------
    // Account map
    // -----------------------------------------------------------------
    py::class_<AccountInfo>(m, "AccountInfo")
        .def(py::init<>())
        .def_readwrite("id",            &AccountInfo::id)
        .def_readwrite("label",         &AccountInfo::label)
        .def_readwrite("venue",         &AccountInfo::venue)
        .def_readwrite("capital_units", &AccountInfo::capital_units)
        .def_readwrite("live",          &AccountInfo::live);

    py::class_<StrategyAccountMap>(m, "StrategyAccountMap")
        .def(py::init<>())
        .def("add_account",   &StrategyAccountMap::add_account,   py::arg("info"))
        .def("assign",        &StrategyAccountMap::assign,
             py::arg("strategy_id"), py::arg("account_id"))
        .def("unassign",      &StrategyAccountMap::unassign,
             py::arg("strategy_id"), py::arg("account_id"))
        .def("primary",       &StrategyAccountMap::primary,       py::arg("strategy_id"))
        .def("accounts_of",   &StrategyAccountMap::accounts_of,   py::arg("strategy_id"))
        .def("strategies_of", &StrategyAccountMap::strategies_of, py::arg("account_id"))
        .def("stamp_primary", &StrategyAccountMap::stamp_primary, py::arg("order"))
        .def_property_readonly("account_count",  &StrategyAccountMap::account_count)
        .def_property_readonly("strategy_count", &StrategyAccountMap::strategy_count);

    // -----------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------
    m.def("to_ticks",   &connector::to_ticks,   py::arg("price"));
    m.def("from_ticks", &connector::from_ticks, py::arg("ticks"));
}
