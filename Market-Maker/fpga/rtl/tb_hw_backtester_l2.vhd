-- MMX — fpga/rtl/tb_hw_backtester_l2.vhd
-- L2 "tester's test" for hw_backtester: instantiates hw_backtester_buggy (the
-- resting-maker BUY fill adds the fill price to cash instead of subtracting it,
-- i.e. a flipped cash-flow sign) and applies the SAME L1-style total_pnl / fills
-- checks that tb_hw_backtester uses. It reports PASS when the injected bug is
-- DETECTED (total_pnl differs from the hand-computed 8), proving the L1 PnL
-- assertion is load-bearing and not vacuous.
--
-- ROM mids (index 0..4): 100, 106, 100, 94, 100  (same as the L1 tb).
-- Good design: total_pnl = 8, fills = 4.
-- Buggy design: BUY fills add price to cash, so total_pnl deviates while fills
-- stays 4 -> the total_pnl check must fire.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity tb_hw_backtester_l2 is
end entity;

architecture sim of tb_hw_backtester_l2 is

    constant TCLK : time := 4 ns;      -- 250 MHz

    signal clk       : std_logic := '0';
    signal rst       : std_logic := '1';
    signal start     : std_logic := '0';
    signal n_ticks   : unsigned(15 downto 0) := (others => '0');
    signal pblk      : param_block_t := PARAM_BLOCK_INIT;
    signal tick_addr : unsigned(15 downto 0);
    signal tick_mid  : tick_t := (others => '0');
    signal done      : std_logic;
    signal total_pnl : signed(63 downto 0);
    signal fills     : unsigned(31 downto 0);

    signal sim_done  : boolean := false;

    -- tick ROM (identical to the L1 testbench)
    constant NROM : natural := 5;
    type rom_t is array (0 to NROM-1) of integer;
    constant ROM : rom_t := (100, 106, 100, 94, 100);

    -- L1 golden expectations
    constant EXP_PNL   : integer := 8;
    constant EXP_FILLS : integer := 4;

begin

    dut : entity work.hw_backtester_buggy
        generic map (NMAX => 1024)
        port map (
            clk       => clk,
            rst       => rst,
            start     => start,
            n_ticks   => n_ticks,
            pblk      => pblk,
            tick_addr => tick_addr,
            tick_mid  => tick_mid,
            done      => done,
            total_pnl => total_pnl,
            fills     => fills
        );

    -- free-running clock
    clk_gen : process
    begin
        while not sim_done loop
            clk <= '0'; wait for TCLK/2;
            clk <= '1'; wait for TCLK/2;
        end loop;
        wait;
    end process;

    -- synchronous BRAM model: 1-cycle registered read latency
    rom_proc : process(clk)
        variable a : integer;
    begin
        if rising_edge(clk) then
            a := to_integer(tick_addr);
            if a >= 0 and a < NROM then
                tick_mid <= to_signed(ROM(a), TICK_W);
            else
                tick_mid <= (others => '0');
            end if;
        end if;
    end process;

    stim : process
        variable mism : integer := 0;
    begin
        -- program the ParamBlock: half_spread=5, zero skews, max_inv=100
        pblk <= (
            inv_skew_q31  => (others => '0'),
            half_spread   => to_signed(5, 32),
            max_inv       => to_unsigned(100, 32),
            base_qty      => to_unsigned(1, 32),
            regime_id     => (others => '0'),
            kill_switch   => '0',
            bid_skew      => (others => '0'),
            ask_skew      => (others => '0'),
            requote_ticks => to_unsigned(1, 16),
            epoch_ns      => (others => '0'),
            valid         => '1');
        n_ticks <= to_unsigned(NROM, 16);

        -- reset
        rst <= '1';
        wait until rising_edge(clk);
        wait until rising_edge(clk);
        rst <= '0';
        wait until rising_edge(clk);

        -- kick off a run (1-cycle start pulse)
        start <= '1';
        wait until rising_edge(clk);
        start <= '0';

        -- wait for completion (bounded)
        for i in 0 to 200 loop
            wait until rising_edge(clk);
            exit when done = '1';
        end loop;
        wait for TCLK/4;

        assert done = '1'
            report "L2: backtester never asserted done" severity failure;

        -- L1-style checks: with the buggy BUY cash-flow sign, total_pnl must
        -- NOT match the golden 8 (fills is expected to still be 4).
        if to_integer(total_pnl) /= EXP_PNL then
            mism := mism + 1;
        end if;

        if mism > 0 then
            report "PASS (L2 tester's test): injected bug CAUGHT -- total_pnl got="
                 & integer'image(to_integer(total_pnl)) & " exp=" & integer'image(EXP_PNL)
                 & " (fills=" & integer'image(to_integer(fills)) & "), so the "
                 & "hw_backtester PnL check is load-bearing (not vacuous)" severity note;
        else
            report "FAIL (L2): injected bug NOT caught -- the hw_backtester PnL check "
                 & "proves nothing!" severity failure;
        end if;

        sim_done <= true;
        wait;
    end process;

end architecture sim;
