-- MMX — fpga/rtl/tb_hw_backtester.vhd
-- Self-checking GHDL testbench for hw_backtester.
--
-- Provides a small internal tick ROM (a rising-then-falling mean-reverting
-- mid path) modelled as a SYNCHRONOUS BRAM (1-cycle read latency), runs the
-- backtester, and asserts total_pnl and fills match a hand-computed value.
--
-- ROM mids (index 0..4): 100, 106, 100, 94, 100
-- ParamBlock: half_spread=5, all skews=0, inv_skew_q31=0, max_inv=100,
--             base_qty=1, kill_switch=0, valid=1.
--   -> compute_quote(mid,inv) = (mid-5, mid+5)  (skew term is 0).
--
-- Hand computation (inv, cash, fills), fill tests the PREVIOUS tick's resting
-- quote; "buy@bid if mid<=bid", "sell@ask if mid>=ask":
--   t0 mid=100 : no resting quote. quote -> bid=95  ask=105
--   t1 mid=106 : 106>=105 -> SELL@105  inv=-1 cash=+105 fills=1  quote 101/111
--   t2 mid=100 : 100<=101 -> BUY @101  inv= 0 cash=  +4 fills=2  quote  95/105
--   t3 mid= 94 :  94<= 95 -> BUY @ 95  inv=+1 cash= -91 fills=3  quote  89/ 99
--   t4 mid=100 : 100>= 99 -> SELL@ 99  inv= 0 cash=  +8 fills=4  quote  95/105
--   mark-to-market: total_pnl = cash + inv*mid_last = 8 + 0*100 = 8
-- EXPECTED: total_pnl = 8, fills = 4

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity tb_hw_backtester is
end entity;

architecture tb of tb_hw_backtester is

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

    -- tick ROM
    constant NROM : natural := 5;
    type rom_t is array (0 to NROM-1) of integer;
    constant ROM : rom_t := (100, 106, 100, 94, 100);

    constant EXP_PNL   : integer := 8;
    constant EXP_FILLS : integer := 4;

begin

    dut : entity work.hw_backtester
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
            report "FAIL: backtester never asserted done" severity failure;

        assert to_integer(total_pnl) = EXP_PNL
            report "FAIL: total_pnl got=" & integer'image(to_integer(total_pnl)) &
                   " exp=" & integer'image(EXP_PNL)
            severity failure;

        assert to_integer(fills) = EXP_FILLS
            report "FAIL: fills got=" & integer'image(to_integer(fills)) &
                   " exp=" & integer'image(EXP_FILLS)
            severity failure;

        report "PASS: hw_backtester total_pnl=" &
               integer'image(to_integer(total_pnl)) &
               " fills=" & integer'image(to_integer(fills)) severity note;

        sim_done <= true;
        wait;
    end process;

end architecture;
