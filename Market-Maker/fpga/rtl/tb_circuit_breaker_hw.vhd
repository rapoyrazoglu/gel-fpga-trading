-- MMX — fpga/rtl/tb_circuit_breaker_hw.vhd
-- Self-checking GHDL testbench for circuit_breaker_hw.
--   Test 1: drive >= cap_thresh distinct cap breaches -> hard_kill=1, reason=01.
--   Test 2: host_reset re-arm clears latch -> hard_kill=0, reason=00.
--   Test 3: drive a PnL drop past pnl_drop_thresh -> hard_kill=1, reason=10.
-- Prints "PASS" only if every check holds; "FAIL ..." + assert otherwise.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity tb_circuit_breaker_hw is end entity;

architecture tb of tb_circuit_breaker_hw is
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
    signal errors : integer := 0;

    procedure tick(signal c : out std_logic) is
    begin
        c <= '0'; wait for TCK/2;
        c <= '1'; wait for TCK/2;
    end procedure;
begin

    dut : entity work.circuit_breaker_hw
        port map(
            clk => clk, rst => rst, inv => inv, max_inv => max_inv,
            pnl => pnl, now_ns => now_ns, cap_thresh => cap_thresh,
            window_ns => window_ns, pnl_drop_thresh => pnl_drop,
            host_reset => host_reset, hard_kill => hard_kill,
            trip_reason => trip_reason);

    stim : process
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
        -- ---- reset ------------------------------------------------------
        rst <= '1'; now_ns <= (others => '0'); pnl <= (others => '0');
        step; step;
        rst <= '0';
        step;

        assert hard_kill = '0'
            report "FAIL: kill asserted out of reset" severity error;
        if hard_kill /= '0' then errors <= errors + 1; end if;

        -- ---- Test 1: 5 distinct cap breaches -> reason 01 ---------------
        -- keep now_ns well inside window so no roll occurs.
        now_ns <= to_unsigned(1000, 64);
        for i in 1 to 5 loop
            cap_pulse;
        end loop;
        -- allow registered outputs to settle
        inv <= (others => '0');
        step; step;

        assert hard_kill = '1'
            report "FAIL: cap storm did not trip hard_kill" severity error;
        if hard_kill /= '1' then errors <= errors + 1; end if;
        assert trip_reason = "01"
            report "FAIL: cap trip reason /= 01" severity error;
        if trip_reason /= "01" then errors <= errors + 1; end if;

        -- latch must hold even after breaches stop
        step; step;
        assert hard_kill = '1'
            report "FAIL: cap kill did not latch" severity error;
        if hard_kill /= '1' then errors <= errors + 1; end if;

        -- ---- Test 2: host_reset re-arms --------------------------------
        host_reset <= '1';
        step;
        host_reset <= '0';
        step;
        assert hard_kill = '0'
            report "FAIL: host_reset did not clear kill" severity error;
        if hard_kill /= '0' then errors <= errors + 1; end if;
        assert trip_reason = "00"
            report "FAIL: reason not cleared after host_reset" severity error;
        if trip_reason /= "00" then errors <= errors + 1; end if;

        -- ---- Test 3: PnL drawdown -> reason 10 -------------------------
        -- baseline pnl=0 captured at re-arm; drop below -pnl_drop (=-1000).
        now_ns <= to_unsigned(2000, 64);   -- still inside window
        inv    <= (others => '0');          -- no cap activity
        pnl    <= to_signed(-1500, 64);     -- delta -1500 < -1000
        step; step;

        assert hard_kill = '1'
            report "FAIL: pnl drop did not trip hard_kill" severity error;
        if hard_kill /= '1' then errors <= errors + 1; end if;
        assert trip_reason = "10"
            report "FAIL: pnl trip reason /= 10" severity error;
        if trip_reason /= "10" then errors <= errors + 1; end if;

        -- ---- verdict ---------------------------------------------------
        if errors = 0 then
            report "PASS" severity note;
        else
            report "FAIL: " & integer'image(errors) & " checks failed"
                severity failure;
        end if;
        wait;
    end process;

end architecture;
