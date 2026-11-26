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
-- consumed is guaranteed stable (no read-before-valid race).
--
-- TIMING: this is a BATCH tool (per-tick latency does not matter, only final
-- PnL after n_ticks), so we PIPELINE the per-tick step across several cycles to
-- close 250 MHz. The old single-cycle S_DATA did fill -> inv -> 64x64 skew
-- multiply -> saturate -> resting quote in ONE cycle (~9.6 ns, WNS -5.578).
-- Split into FILL | MULT | RESV | QUOTE stages, and NARROW the skew multiply
-- from 64x64 to 34x32 (inventory is clamped to +/-max_inv ~= 32-bit, param is
-- 32-bit) exactly like pricing_engine_beh. Bit-exact, timing MET.
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

entity hw_backtester is
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

architecture rtl of hw_backtester is

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

    -- FSM: per tick we walk ADDR -> WAIT -> FILL -> MULT -> RESV -> QUOTE.
    -- The compute is pipelined so no single cycle carries the long
    -- fill->multiply->saturate chain that broke timing.
    type state_t is (S_IDLE, S_ADDR, S_WAIT, S_FILL, S_MULT, S_RESV, S_QUOTE, S_DONE);

    signal state       : state_t := S_IDLE;
    signal idx         : unsigned(15 downto 0) := (others => '0');

    -- accumulators
    signal inv         : signed(31 downto 0)  := (others => '0');  -- lots
    signal cash        : signed(63 downto 0)  := (others => '0');  -- signed cash flow
    signal fills_r     : unsigned(31 downto 0) := (others => '0');

    -- per-tick pipeline registers (FILL -> MULT -> RESV -> QUOTE)
    signal mid_reg     : tick_t          := (others => '0');  -- this tick's mid, held
    signal qc_reg      : signed(33 downto 0) := (others => '0'); -- clamped inv (34-bit)
    signal kill_reg    : std_logic       := '0';
    signal last_reg    : std_logic       := '0';             -- this is the last tick
    signal prod_reg    : signed(65 downto 0) := (others => '0'); -- 34x32 skew product
    signal r_reg       : signed(63 downto 0) := (others => '0'); -- reservation price
    signal mm_reg      : signed(63 downto 0) := (others => '0'); -- inv*mid mark product

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
        variable cap32   : signed(31 downto 0);
        variable qc_v    : signed(31 downto 0);
        variable skew    : signed(63 downto 0);
        variable bid_v   : signed(63 downto 0);
        variable ask_v   : signed(63 downto 0);
    begin
        if rising_edge(clk) then
            if rst = '1' then
                state       <= S_IDLE;
                idx         <= (others => '0');
                inv         <= (others => '0');
                cash        <= (others => '0');
                fills_r     <= (others => '0');
                mid_reg     <= (others => '0');
                qc_reg      <= (others => '0');
                kill_reg    <= '0';
                last_reg    <= '0';
                prod_reg    <= (others => '0');
                r_reg       <= (others => '0');
                mm_reg      <= (others => '0');
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
                            idx         <= (others => '0');
                            inv         <= (others => '0');
                            cash        <= (others => '0');
                            fills_r     <= (others => '0');
                            rest_valid  <= '0';
                            rest_cancel <= '0';
                            pnl_r       <= (others => '0');
                            if n_ticks = 0 then
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
                        state <= S_FILL;

                    -- ==== STAGE FILL: resting-maker fill + commit accumulators ====
                    when S_FILL =>
                        inv_v   := inv;
                        cash_v  := cash;
                        fills_v := fills_r;
                        m_v     := tick_mid;
                        cap64   := signed(resize(pblk.max_inv, 64));
                        cap32   := signed(resize(pblk.max_inv, 32));

                        if (rest_valid = '1') and (rest_cancel = '0') then
                            if (m_v <= rest_bid) and
                               (resize(inv_v, 64) < cap64) then
                                inv_v   := inv_v + 1;                       -- BUY 1
                                cash_v  := cash_v - resize(rest_bid, 64);
                                fills_v := fills_v + 1;
                            elsif (m_v >= rest_ask) and
                                  (resize(inv_v, 64) > -cap64) then
                                inv_v   := inv_v - 1;                       -- SELL 1
                                cash_v  := cash_v + resize(rest_ask, 64);
                                fills_v := fills_v + 1;
                            end if;
                        end if;

                        inv     <= inv_v;
                        cash    <= cash_v;
                        fills_r <= fills_v;
                        mid_reg <= m_v;

                        -- kill + clamp qc for the multiply stage (post-fill inv)
                        kill_reg <= pblk.kill_switch;
                        if (resize(inv_v, 64) > cap64) or
                           (resize(inv_v, 64) < -cap64) then
                            kill_reg <= '1';
                        end if;
                        qc_v := inv_v;
                        if    qc_v >  cap32 then qc_v :=  cap32;
                        elsif qc_v < -cap32 then qc_v := -cap32;
                        end if;
                        qc_reg <= resize(qc_v, 34);

                        if (idx + 1) = n_ticks then last_reg <= '1';
                        else                        last_reg <= '0'; end if;

                        state <= S_MULT;

                    -- ==== STAGE MULT: narrowed 34x32 skew multiply ================
                    when S_MULT =>
                        prod_reg <= qc_reg * resize(pblk.inv_skew_q31, 32);
                        state    <= S_RESV;

                    -- ==== STAGE RESV: reservation price + mark-to-market product ==
                    when S_RESV =>
                        skew  := shift_right(resize(prod_reg, 64) + ROUND_2P30, 31);
                        r_reg <= resize(mid_reg, 64) - skew;
                        mm_reg <= resize(inv * mid_reg, 64);  -- parallel mark product
                        state <= S_QUOTE;

                    -- ==== STAGE QUOTE: bid/ask + saturate + commit / finish =======
                    when S_QUOTE =>
                        bid_v := r_reg - resize(pblk.half_spread, 64)
                                       - resize(pblk.bid_skew, 64);
                        ask_v := r_reg + resize(pblk.half_spread, 64)
                                       + resize(pblk.ask_skew, 64);
                        rest_bid    <= sat_i32(bid_v);
                        rest_ask    <= sat_i32(ask_v);
                        rest_cancel <= kill_reg;
                        rest_valid  <= '1';

                        if last_reg = '1' then
                            pnl_r  <= cash + mm_reg;   -- realized + marked-to-market
                            done_r <= '1';
                            state  <= S_DONE;
                        else
                            idx   <= idx + 1;
                            state <= S_ADDR;
                        end if;

                    when S_DONE =>
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
