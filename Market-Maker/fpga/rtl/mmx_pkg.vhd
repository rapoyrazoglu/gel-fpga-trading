-- MMX — fpga/rtl/mmx_pkg.vhd
-- Shared types/constants for the Phase B data plane (spec §4.1-4.8).
-- VHDL-2008. This package is the interface CONTRACT every RTL module obeys so
-- the pipeline stages connect without drift:
--
--   mac_udp_rx -> feed_parser -> order_book --s--> [pricing_engine HLS] -> quote_decider -> order_encoder
--                                    ^                     ^
--   inventory_counter --q-----------(q)-------------------/
--        ^ (own fills)
--   param_block_loader (AXI-Lite from CPU) --ParamBlock--> pricing_engine + quote_decider
--
-- Mirrors host/ipc/param_block.h and host/pricing/pricing_engine.h. Internal
-- pipeline payloads are VHDL records (synthesizable in Vivado); the external
-- MAC boundary uses byte-packed AXI-Stream (tdata/tkeep/tlast/tvalid/tready).

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package mmx_pkg is

    -- ---- synthesis-time constants -------------------------------------------
    constant CLK_PERIOD_NS   : real    := 4.0;          -- 250 MHz
    constant N_LEVELS        : natural := 4;            -- order book depth/side
    constant TICK_W          : natural := 32;           -- price/inventory, signed
    constant QTY_W           : natural := 32;           -- size, unsigned
    constant TS_W            : natural := 64;           -- ns timestamp
    constant TICKER_W        : natural := 16;
    constant AXIS_W          : natural := 64;           -- MAC datapath width

    -- §7.3 staleness 500 ms; §3.5 max quote age 50 ms (in ns).
    constant STALENESS_NS    : unsigned(63 downto 0) := to_unsigned(500000000, 64);
    constant MAX_QUOTE_AGE_NS: unsigned(63 downto 0) := to_unsigned( 50000000, 64);

    subtype tick_t is signed(TICK_W-1 downto 0);
    subtype qty_t  is unsigned(QTY_W-1 downto 0);
    subtype ts_t   is unsigned(TS_W-1 downto 0);

    type side_t is (BID, ASK);

    -- ITCH-lite message type codes (feed_parser).
    constant MSG_ADD   : std_logic_vector(7 downto 0) := x"41"; -- 'A' add order
    constant MSG_DEL   : std_logic_vector(7 downto 0) := x"44"; -- 'D' delete
    constant MSG_EXEC  : std_logic_vector(7 downto 0) := x"45"; -- 'E' execute (our fill)
    constant MSG_TRADE : std_logic_vector(7 downto 0) := x"50"; -- 'P' trade

    -- ---- normalized feed message (feed_parser -> order_book / inventory) -----
    type feed_msg_t is record
        ts_ns     : ts_t;
        ticker_id : std_logic_vector(TICKER_W-1 downto 0);
        side      : side_t;
        price     : tick_t;
        size      : qty_t;
        msg_type  : std_logic_vector(7 downto 0);
    end record;

    -- ---- order_book output ---------------------------------------------------
    type book_out_t is record
        ts_ns     : ts_t;
        mid       : tick_t;            -- (best_bid + best_ask)/2
        spread    : tick_t;            -- best_ask - best_bid
        imbalance : signed(15 downto 0); -- Q1.15 (bidQ-askQ)/(bidQ+askQ)
    end record;

    -- ---- ParamBlock mirror (HMAC-protected fields, param_block.h) ------------
    type param_block_t is record
        inv_skew_q31  : signed(31 downto 0);
        half_spread   : signed(31 downto 0);
        max_inv       : unsigned(31 downto 0);
        base_qty      : unsigned(31 downto 0);
        regime_id     : std_logic_vector(7 downto 0);
        kill_switch   : std_logic;
        bid_skew      : signed(15 downto 0);
        ask_skew      : signed(15 downto 0);
        requote_ticks : unsigned(15 downto 0);
        epoch_ns      : unsigned(63 downto 0);
        valid         : std_logic;
    end record;

    constant PARAM_BLOCK_INIT : param_block_t := (
        inv_skew_q31  => (others => '0'),
        half_spread   => (others => '0'),
        max_inv       => to_unsigned(100, 32),
        base_qty      => to_unsigned(1, 32),
        regime_id     => (others => '0'),
        kill_switch   => '1',           -- safe default: cancel-all until loaded
        bid_skew      => (others => '0'),
        ask_skew      => (others => '0'),
        requote_ticks => to_unsigned(1, 16),
        epoch_ns      => (others => '0'),
        valid         => '0');

    -- ---- pricing_engine output / quote (matches QuoteOut) --------------------
    type quote_t is record
        bid         : tick_t;
        ask         : tick_t;
        qty         : qty_t;
        cancel_only : std_logic;
    end record;

    -- ---- order action (quote_decider -> order_encoder) -----------------------
    type order_op_t is (OP_NONE, OP_PLACE, OP_CANCEL, OP_REPLACE);
    type order_action_t is record
        op  : order_op_t;
        bid : tick_t;
        ask : tick_t;
        qty : qty_t;
        ref : std_logic_vector(31 downto 0);  -- ref order id for cancel/replace
    end record;

    -- ---- fill report (RX -> inventory_counter) -------------------------------
    type fill_t is record
        valid : std_logic;
        side  : side_t;   -- BID = our bid filled (we bought), ASK = we sold
        qty   : qty_t;
        price : tick_t;
    end record;

    -- ---- helpers -------------------------------------------------------------
    function mid_of  (best_bid, best_ask : tick_t) return tick_t;
    function sat_add (a : tick_t; b : tick_t; lo, hi : tick_t) return tick_t;

end package mmx_pkg;

package body mmx_pkg is

    function mid_of(best_bid, best_ask : tick_t) return tick_t is
        variable s : signed(TICK_W downto 0);
    begin
        s := resize(best_bid, TICK_W+1) + resize(best_ask, TICK_W+1);
        return resize(shift_right(s, 1), TICK_W);  -- arithmetic /2
    end function;

    -- saturating add with explicit [lo,hi] clamp (inventory cap).
    function sat_add(a : tick_t; b : tick_t; lo, hi : tick_t) return tick_t is
        variable s : signed(TICK_W downto 0);
    begin
        s := resize(a, TICK_W+1) + resize(b, TICK_W+1);
        if    s > resize(hi, TICK_W+1) then return hi;
        elsif s < resize(lo, TICK_W+1) then return lo;
        else  return resize(s, TICK_W);
        end if;
    end function;

end package body mmx_pkg;
