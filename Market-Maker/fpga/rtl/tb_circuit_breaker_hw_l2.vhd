-- MMX — fpga/rtl/tb_circuit_breaker_hw_l2.vhd
-- L2 "tester's test" for circuit_breaker_hw: instantiates circuit_breaker_hw_buggy
-- (uses a strict ">" instead of ">=" when comparing the rolling breach counter to
-- cap_thresh, an off-by-one that needs cap_thresh+1 breaches to trip) and applies
-- the L1-style cap-storm check. It reports PASS when the injected bug is DETECTED
-- (after exactly cap_thresh=5 distinct breaches the buggy breaker fails to latch
-- hard_kill / reason=01), proving the L1 cap-storm assertion is load-bearing and
-- not vacuous.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity tb_circuit_breaker_hw_l2 is end entity;

architecture sim of tb_circuit_breaker_hw_l2 is
    signal clk         : std_logic := '0';
    signal rst         : std_logic := '1';
    signal inv         : tick_t    := (others => '0');
    signal max_inv     : unsigned(31 downto 0) := to_unsigned(100, 32);
    signal pnl         : signed(63 downto 0)   := (others => '0');
    signal now_ns      : ts_t      := (others => '0');
    signal cap_thresh  : unsigned(7 downto 0)  := to_unsigned(5, 8);
    signal window_ns   : ts_t      := to_unsigned(1000000000, 64); -- 1e9 ns
    signal pnl_drop    : signed(63 downto 0)   := to_signed(1000, 64);
    signal host_reset  : std_logic := '0';
    signal hard_kill   : std_logic;
    signal trip_reason : std_logic_vector(1 downto 0);

    constant TCK : time := 4 ns;

    procedure tick(signal c : out std_logic) is
    begin
        c <= '0'; wait for TCK/2;
        c <= '1'; wait for TCK/2;
    end procedure;
begin

    dut : entity work.circuit_breaker_hw_buggy
        port map(
            clk => clk, rst => rst, inv => inv, max_inv => max_inv,
            pnl => pnl, now_ns => now_ns, cap_thresh => cap_thresh,
            window_ns => window_ns, pnl_drop_thresh => pnl_drop,
            host_reset => host_reset, hard_kill => hard_kill,
            trip_reason => trip_reason);

    stim : process
        variable mism : integer := 0;

        procedure step is begin tick(clk); end procedure;

        -- generate one distinct cap breach: |inv|>=max_inv for a cycle, then
        -- back under so the next breach is a fresh rising edge.
        procedure cap_pulse is
        begin
            inv <= to_signed(200, 32);   -- |200| >= 100
            step;
            inv <= to_signed(10, 32);    -- back under cap
            step;
        end procedure;
    begin
        -- ---- reset (same drive sequence as the L1 tb) ------------------
        rst <= '1'; now_ns <= (others => '0'); pnl <= (others => '0');
        step; step;
        rst <= '0';
        step;

        -- ---- L1-style cap-storm exercise: 5 distinct breaches ----------
        -- keep now_ns well inside window so no roll occurs.
        now_ns <= to_unsigned(1000, 64);
        for i in 1 to 5 loop
            cap_pulse;
        end loop;
        -- allow registered outputs to settle
        inv <= (others => '0');
        step; step;

        -- L1-style checks: cap_thresh=5 distinct breaches MUST trip hard_kill
        -- with reason "01". The buggy off-by-one ">" fails to latch here.
        if hard_kill /= '1' then
            mism := mism + 1;
        end if;
        if trip_reason /= "01" then
            mism := mism + 1;
        end if;

        step; step;

        if mism > 0 then
            report "PASS (L2 tester's test): injected bug CAUGHT -- after "
                 & "cap_thresh=5 distinct inventory breaches circuit_breaker_hw_buggy "
                 & "did NOT latch hard_kill/reason=01 (strict '>' off-by-one), so the "
                 & "L1 cap-storm check is load-bearing" severity note;
        else
            report "FAIL (L2): injected bug NOT caught -- the circuit_breaker_hw "
                 & "cap-storm check proves nothing!" severity failure;
        end if;

        wait;
    end process;

end architecture sim;
