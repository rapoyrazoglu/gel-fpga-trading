-- MMX — fpga/rtl/circuit_breaker_hw.vhd
-- Hardware drop-circuit / kill breaker (spec §7 risk plane).
--
-- Independent, always-on safety latch that sits between the quote path and the
-- order_encoder. It watches two orthogonal risk signals over a rolling time
-- window and, when either exceeds its cap, latches hard_kill='1' so the data
-- plane can gate all new orders with zero combinational dependence on the CPU.
--
--   1) Inventory-cap storm : |inv| >= max_inv, rising-edge counted. If the
--      number of distinct cap breaches inside the current window_ns reaches
--      cap_thresh -> trip, trip_reason = "01".
--   2) PnL drawdown        : (pnl - pnl_at_window_start) < -pnl_drop_thresh
--      inside the current window -> trip, trip_reason = "10".
--
-- The window is a simple tumbling window anchored at window_start; when
-- now_ns - window_start >= window_ns it rolls, re-arming the cap counter and
-- re-sampling the PnL baseline. Once tripped, hard_kill and trip_reason latch
-- until host_reset='1' (or the global sync rst). INTEGER-ONLY, numeric_std,
-- single clock, sync active-high reset, registered outputs, no latches.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity circuit_breaker_hw is
    port(
        clk, rst        : in  std_logic;
        inv             : in  tick_t;                       -- signed32 inventory
        max_inv         : in  unsigned(31 downto 0);        -- cap magnitude
        pnl             : in  signed(63 downto 0);          -- marked PnL
        now_ns          : in  ts_t;                         -- unsigned64 clock (ns)
        cap_thresh      : in  unsigned(7 downto 0);         -- breaches/window to trip
        window_ns       : in  ts_t;                         -- rolling window length
        pnl_drop_thresh : in  signed(63 downto 0);          -- max loss per window (>=0)
        host_reset      : in  std_logic;                    -- CPU re-arm
        hard_kill       : out std_logic;                    -- LATCHED once tripped
        trip_reason     : out std_logic_vector(1 downto 0)  -- 00 none/01 cap/10 pnl
    );
end entity;

architecture rtl of circuit_breaker_hw is

    signal kill_i          : std_logic               := '0';
    signal reason_i        : std_logic_vector(1 downto 0) := "00";
    signal cap_count       : unsigned(15 downto 0)   := (others => '0');
    signal window_start    : ts_t                    := (others => '0');
    signal pnl_win_start   : signed(63 downto 0)     := (others => '0');
    signal prev_cap_over   : std_logic               := '0';

begin

    hard_kill   <= kill_i;
    trip_reason <= reason_i;

    process(clk)
        variable inv_abs   : signed(32 downto 0);
        variable max_s     : signed(32 downto 0);
        variable cap_over  : std_logic;
        variable cap_event : std_logic;
        variable new_count : unsigned(15 downto 0);
        variable pnl_delta : signed(63 downto 0);
        variable neg_thr   : signed(63 downto 0);
    begin
        if rising_edge(clk) then

            -- |inv| >= max_inv, evaluated as a 33-bit signed magnitude compare.
            inv_abs := abs(resize(inv, 33));
            max_s   := signed('0' & max_inv);
            if inv_abs >= max_s then
                cap_over := '1';
            else
                cap_over := '0';
            end if;
            -- rising-edge detect: count each distinct breach once.
            cap_event := cap_over and (not prev_cap_over);

            if (rst = '1') or (host_reset = '1') then
                -- global sync reset OR CPU re-arm: clear the latch and re-anchor.
                kill_i        <= '0';
                reason_i      <= "00";
                cap_count     <= (others => '0');
                window_start  <= now_ns;
                pnl_win_start <= pnl;
                prev_cap_over <= '0';
            else
                prev_cap_over <= cap_over;

                pnl_delta := pnl - pnl_win_start;
                neg_thr   := -pnl_drop_thresh;

                if (now_ns >= window_start) and
                   ((now_ns - window_start) >= window_ns) then
                    -- tumbling window rolls: re-anchor baseline + counter.
                    window_start  <= now_ns;
                    pnl_win_start <= pnl;
                    if cap_event = '1' then
                        cap_count <= to_unsigned(1, cap_count'length);
                    else
                        cap_count <= (others => '0');
                    end if;
                    -- fresh window: no trip can occur on the roll cycle.
                else
                    if cap_event = '1' then
                        new_count := cap_count + 1;
                    else
                        new_count := cap_count;
                    end if;
                    cap_count <= new_count;

                    -- evaluate trips only while not already latched.
                    if kill_i = '0' then
                        if new_count >= resize(cap_thresh, new_count'length) then
                            kill_i   <= '1';
                            reason_i <= "01";        -- inventory-cap storm
                        elsif pnl_delta < neg_thr then
                            kill_i   <= '1';
                            reason_i <= "10";        -- PnL drawdown
                        end if;
                    end if;
                end if;
            end if;
        end if;
    end process;

end architecture;
