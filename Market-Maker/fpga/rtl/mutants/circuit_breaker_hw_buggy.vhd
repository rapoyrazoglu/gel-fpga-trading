-- MMX — fpga/rtl/circuit_breaker_hw_buggy.vhd
-- L2 "tester's test" fixture: a DELIBERATELY BUGGY copy of circuit_breaker_hw.
--
-- This file is NOT part of the shipping design. tb_circuit_breaker_hw_l2 drives it
-- and asserts the L1-style cap-storm check CATCHES the bug (L2 PASS == bug detected).
--
-- ---- INJECTED BUG (exactly one) ---------------------------------------------
-- The cap-storm trip compares the breach counter against cap_thresh. The spec
-- (and L1 test) require a trip when the count REACHES cap_thresh, i.e. a ">="
-- compare. This buggy copy uses a strict ">" instead (off-by-one), so it needs
-- cap_thresh+1 breaches to trip. With exactly cap_thresh breaches the breaker
-- fails to latch hard_kill. Everything else is a verbatim copy of
-- circuit_breaker_hw.vhd. See the "L2 INJECTED BUG" marker below.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity circuit_breaker_hw_buggy is
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

architecture rtl of circuit_breaker_hw_buggy is

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
                        -- L2 INJECTED BUG: should be ">=" (trip when the breach
                        -- count REACHES cap_thresh); this strict ">" is an
                        -- off-by-one that needs cap_thresh+1 breaches to trip.
                        if new_count > resize(cap_thresh, new_count'length) then
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
