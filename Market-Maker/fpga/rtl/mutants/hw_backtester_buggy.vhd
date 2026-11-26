-- MMX — fpga/rtl/hw_backtester.vhd
-- Hardware backtester (the showcase, spec §Phase-B tooling).
--
-- Streams a historical mid-price series out of an external BRAM (0..n_ticks-1),
-- runs the SAME integer pricing math as pricing_engine_beh / mmx::compute_quote
-- per tick, applies a SIMPLE resting-maker fill model, and accumulates a
-- mark-to-market PnL and a fill counter. When the last tick is processed it
-- raises `done` with the final `total_pnl` and `fills`.
--
-- BRAM contract: drive tick_addr this cycle, the addressed mid is presented on
-- tick_mid one cycle later (synchronous / registered BRAM read). The FSM below
-- honours that latency with an explicit ADDR -> WAIT -> DATA loop so the value
-- consumed in DATA is guaranteed stable (no read-before-valid race).
--
-- Fill model (maker, one lot per tick, respecting +/- max_inv):
--   * After processing tick i we compute compute_quote(mid_i, inv) and let that
--     (bid,ask) REST. The next tick's mid tests the resting quote:
--       mid_{i+1} <= rest_bid  and inv <  +max_inv  -> we BUY 1  @ rest_bid
--       mid_{i+1} >= rest_ask  and inv >  -max_inv  -> we SELL 1 @ rest_ask
--     (at most one fill per tick; cancel_only quotes never fill).
--   * cash accumulates signed cash flow (-buy price / +sell price).
--   * total_pnl = cash + inv * mid_last   (realized + marked-to-market).
--
-- Integer-only, numeric_std only, single clock, synchronous active-high reset,
-- registered outputs, no inferred latches. Synthesizable VHDL-2008.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity hw_backtester_buggy is
    generic (
        NMAX : natural := 1024               -- max #ticks the host BRAM can hold
    );
    port (
        clk       : in  std_logic;
        rst       : in  std_logic;           -- synchronous, active-high
        start     : in  std_logic;           -- 1-cycle pulse kicks off a run
        n_ticks   : in  unsigned(15 downto 0);
        pblk      : in  param_block_t;
        tick_addr : out unsigned(15 downto 0);            -- BRAM read address
        tick_mid  : in  tick_t;                           -- BRAM data, 1-cycle late
        done      : out std_logic;
        total_pnl : out signed(63 downto 0);
        fills     : out unsigned(31 downto 0)
    );
end entity;

architecture rtl of hw_backtester_buggy is

    -- int32 saturation bounds (mirror of pricing_engine_beh).
    constant INT32_MAX_64 : signed(63 downto 0) := to_signed(2147483647, 64);
    constant INT32_MIN_64 : signed(63 downto 0) := to_signed(-2147483647, 64) - 1;
    constant INT32_MAX_32 : tick_t              := to_signed(2147483647, TICK_W);
    constant INT32_MIN_32 : tick_t              := to_signed(-2147483647, TICK_W) - 1;
    constant ROUND_2P30   : signed(63 downto 0) := to_signed(1073741824, 64); -- 2^30

    -- NMAX only bounds the host ROM; referenced here to keep the generic live.
    constant NMAX_C : natural := NMAX;

    function sat_i32(v : signed(63 downto 0)) return tick_t is
    begin
        if    v > INT32_MAX_64 then return INT32_MAX_32;
        elsif v < INT32_MIN_64 then return INT32_MIN_32;
        else  return resize(v, TICK_W);
        end if;
    end function;

    -- Pure combinational mirror of compute_quote() / pricing_engine_beh.
    function compute_quote_f(s : tick_t; q : tick_t; p : param_block_t)
        return quote_t is
        variable cap_s   : signed(63 downto 0);
        variable q64     : signed(63 downto 0);
        variable qc      : signed(63 downto 0);
        variable prod128 : signed(127 downto 0);
        variable prod    : signed(63 downto 0);
        variable skew    : signed(63 downto 0);
        variable r       : signed(63 downto 0);
        variable bid_v   : signed(63 downto 0);
        variable ask_v   : signed(63 downto 0);
        variable kill    : std_logic;
        variable res     : quote_t;
    begin
        cap_s := signed(resize(p.max_inv, 64));
        q64   := resize(q, 64);

        kill := p.kill_switch;
        if (q64 > cap_s) or (q64 < -cap_s) then
            kill := '1';
        end if;

        qc := q64;
        if    qc >  cap_s then qc :=  cap_s;
        elsif qc < -cap_s then qc := -cap_s;
        end if;

        prod128 := qc * resize(p.inv_skew_q31, 64);
        prod    := resize(prod128, 64);
        skew    := shift_right(prod + ROUND_2P30, 31);
        r       := resize(s, 64) - skew;

        bid_v := r - resize(p.half_spread, 64) - resize(p.bid_skew, 64);
        ask_v := r + resize(p.half_spread, 64) + resize(p.ask_skew, 64);

        res.bid         := sat_i32(bid_v);
        res.ask         := sat_i32(ask_v);
        res.qty         := p.base_qty;
        res.cancel_only := kill;
        return res;
    end function;

    type state_t is (S_IDLE, S_ADDR, S_WAIT, S_DATA, S_DONE);

    signal state       : state_t := S_IDLE;
    signal idx         : unsigned(15 downto 0) := (others => '0');

    -- accumulators
    signal inv         : signed(31 downto 0)  := (others => '0');  -- lots
    signal cash        : signed(63 downto 0)  := (others => '0');  -- signed cash flow
    signal fills_r     : unsigned(31 downto 0) := (others => '0');

    -- resting quote (from previous tick)
    signal rest_valid  : std_logic := '0';
    signal rest_cancel : std_logic := '0';
    signal rest_bid    : tick_t    := (others => '0');
    signal rest_ask    : tick_t    := (others => '0');

    -- output registers
    signal tick_addr_r : unsigned(15 downto 0) := (others => '0');
    signal done_r      : std_logic := '0';
    signal pnl_r       : signed(63 downto 0) := (others => '0');

begin

    tick_addr <= tick_addr_r;
    done      <= done_r;
    total_pnl <= pnl_r;
    fills     <= fills_r;

    process(clk)
        variable inv_v   : signed(31 downto 0);
        variable cash_v  : signed(63 downto 0);
        variable fills_v : unsigned(31 downto 0);
        variable m_v     : tick_t;
        variable cap64   : signed(63 downto 0);
        variable qv      : quote_t;
        variable prod    : signed(63 downto 0);
    begin
        if rising_edge(clk) then
            if rst = '1' then
                state       <= S_IDLE;
                idx         <= (others => '0');
                inv         <= (others => '0');
                cash        <= (others => '0');
                fills_r     <= (others => '0');
                rest_valid  <= '0';
                rest_cancel <= '0';
                rest_bid    <= (others => '0');
                rest_ask    <= (others => '0');
                tick_addr_r <= (others => '0');
                done_r      <= '0';
                pnl_r       <= (others => '0');
            else
                case state is

                    when S_IDLE =>
                        done_r <= '0';
                        if start = '1' then
                            -- fresh run: clear accumulators
                            idx         <= (others => '0');
                            inv         <= (others => '0');
                            cash        <= (others => '0');
                            fills_r     <= (others => '0');
                            rest_valid  <= '0';
                            rest_cancel <= '0';
                            pnl_r       <= (others => '0');
                            if n_ticks = 0 then
                                -- empty series: nothing to do
                                done_r <= '1';
                                state  <= S_DONE;
                            else
                                state <= S_ADDR;
                            end if;
                        end if;

                    when S_ADDR =>
                        tick_addr_r <= idx;      -- present address to BRAM
                        state       <= S_WAIT;

                    when S_WAIT =>
                        -- BRAM registers the address this cycle; data valid next.
                        state <= S_DATA;

                    when S_DATA =>
                        -- tick_mid now holds mid for index `idx` (stable).
                        inv_v   := inv;
                        cash_v  := cash;
                        fills_v := fills_r;
                        m_v     := tick_mid;
                        cap64   := signed(resize(pblk.max_inv, 64));

                        -- resting-maker fill (at most one per tick)
                        if (rest_valid = '1') and (rest_cancel = '0') then
                            if (m_v <= rest_bid) and
                               (resize(inv_v, 64) < cap64) then
                                -- our bid filled: we BUY 1 lot @ rest_bid
                                -- INJECTED BUG: buy cash flow sign flipped (+ instead of -);
                                -- a buy should DECREASE cash by the fill price.
                                inv_v   := inv_v + 1;
                                cash_v  := cash_v + resize(rest_bid, 64);
                                fills_v := fills_v + 1;
                            elsif (m_v >= rest_ask) and
                                  (resize(inv_v, 64) > -cap64) then
                                -- our ask filled: we SELL 1 lot @ rest_ask
                                inv_v   := inv_v - 1;
                                cash_v  := cash_v + resize(rest_ask, 64);
                                fills_v := fills_v + 1;
                            end if;
                        end if;

                        -- new resting quote from this mid + post-fill inventory
                        qv := compute_quote_f(m_v, inv_v, pblk);

                        -- commit accumulators
                        inv         <= inv_v;
                        cash        <= cash_v;
                        fills_r     <= fills_v;
                        rest_bid    <= qv.bid;
                        rest_ask    <= qv.ask;
                        rest_cancel <= qv.cancel_only;
                        rest_valid  <= '1';

                        -- advance / finish
                        if (idx + 1) = n_ticks then
                            -- mark to market on the last mid: inv * mid_last
                            prod   := resize(inv_v * m_v, 64);
                            pnl_r  <= cash_v + prod;
                            done_r <= '1';
                            state  <= S_DONE;
                        else
                            idx   <= idx + 1;
                            state <= S_ADDR;
                        end if;

                    when S_DONE =>
                        -- latch done + results until a new run is requested
                        done_r <= '1';
                        if start = '1' then
                            idx         <= (others => '0');
                            inv         <= (others => '0');
                            cash        <= (others => '0');
                            fills_r     <= (others => '0');
                            rest_valid  <= '0';
                            rest_cancel <= '0';
                            pnl_r       <= (others => '0');
                            done_r      <= '0';
                            if n_ticks = 0 then
                                done_r <= '1';
                            else
                                state <= S_ADDR;
                            end if;
                        end if;

                    when others =>
                        state <= S_IDLE;

                end case;
            end if;
        end if;
    end process;

end architecture;
