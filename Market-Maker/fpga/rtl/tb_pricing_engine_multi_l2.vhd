-- MMX — fpga/rtl/tb_pricing_engine_multi_l2.vhd
-- L2 TESTER'S-TEST for pricing_engine_multi.
-- Instantiates the deliberately BUGGY wrapper pricing_engine_multi_buggy as the
-- DUT and drives it exactly the way tb_pricing_engine_multi drives the good
-- pricing_engine_multi. The golden references are UNMODIFIED single-lane
-- pricing_engine_beh cores. It applies the same L1-style per-lane parity checks
-- and reports:
--   PASS (L2): the bug was DETECTED (at least one lane mismatched the reference),
--              proving the L1 test is load-bearing (not vacuous).
--   FAIL (L2): the buggy DUT matched the reference -> L1 test proves nothing.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;
use work.mmx_multi_pkg.all;

entity tb_pricing_engine_multi_l2 is end entity;

architecture tb of tb_pricing_engine_multi_l2 is

    constant N : natural := 4;

    signal clk         : std_logic := '0';
    signal rst         : std_logic := '1';
    signal valid_in    : std_logic := '0';

    signal s_in        : tick_vec_t(0 to N-1) := (others => (others => '0'));
    signal q_in        : tick_vec_t(0 to N-1) := (others => (others => '0'));
    signal pblk        : pb_vec_t(0 to N-1)   := (others => PARAM_BLOCK_INIT);

    signal quote_out   : quote_vec_t(0 to N-1);
    signal quote_valid : std_logic;

    -- Golden single-lane (GOOD) reference per-lane outputs.
    signal ref_quote   : quote_vec_t(0 to N-1);
    signal ref_valid   : std_logic_vector(0 to N-1);

    constant HALF : time := 2 ns;   -- 250 MHz
    signal done  : boolean := false;

begin

    -- clock
    clk <= not clk after HALF when not done else '0';

    -- DUT = BUGGY multi-lane wrapper
    dut : entity work.pricing_engine_multi_buggy
        generic map ( N => N )
        port map (
            clk => clk, rst => rst,
            s_in => s_in, q_in => q_in,
            valid_in => valid_in, pblk => pblk,
            quote_out => quote_out, quote_valid => quote_valid
        );

    -- Golden single-lane references (GOOD core, one per lane).
    gen_ref : for i in 0 to N-1 generate
        u_ref : entity work.pricing_engine_beh
            port map (
                clk => clk, rst => rst,
                s_in => s_in(i), q_in => q_in(i),
                valid_in => valid_in, pblk => pblk(i),
                quote_out => ref_quote(i), quote_valid => ref_valid(i)
            );
    end generate;

    stim : process
        -- Build a distinct ParamBlock for a lane.
        function mk_pb(hs, invskew, bskew, askew : integer;
                       maxinv, baseq : integer; kill : std_logic)
            return param_block_t is
            variable p : param_block_t := PARAM_BLOCK_INIT;
        begin
            p.half_spread  := to_signed(hs, 32);
            p.inv_skew_q31 := to_signed(invskew, 32);
            p.bid_skew     := to_signed(bskew, 16);
            p.ask_skew     := to_signed(askew, 16);
            p.max_inv      := to_unsigned(maxinv, 32);
            p.base_qty     := to_unsigned(baseq, 32);
            p.kill_switch  := kill;
            p.valid        := '1';
            return p;
        end function;

        procedure step is
        begin
            wait until rising_edge(clk);
        end procedure;

        variable errors : integer := 0;

        -- Same L1-style checks, but COUNT mismatches instead of asserting
        -- failure (mismatches are the expected/desired outcome here).
        procedure check_all is
        begin
            for i in 0 to N-1 loop
                if quote_out(i).bid /= ref_quote(i).bid
                   or quote_out(i).ask /= ref_quote(i).ask
                   or quote_out(i).qty /= ref_quote(i).qty
                   or quote_out(i).cancel_only /= ref_quote(i).cancel_only then
                    errors := errors + 1;
                end if;
            end loop;
        end procedure;

    begin
        -- reset for a couple cycles
        rst <= '1'; valid_in <= '0';
        step; step;
        rst <= '0';

        -- ---- vector 0 : distinct lanes ------------------------------------
        s_in(0) <= to_signed( 100000, 32); q_in(0) <= to_signed(   10, 32);
        pblk(0) <= mk_pb(50, 1000000, 3, 4, 100, 5, '0');
        s_in(1) <= to_signed( 250000, 32); q_in(1) <= to_signed(  -25, 32);
        pblk(1) <= mk_pb(75,  500000, -2, 1, 200, 7, '0');
        s_in(2) <= to_signed(  50000, 32); q_in(2) <= to_signed(  999, 32);
        pblk(2) <= mk_pb(20, 2000000, 0, 0,  50, 3, '0');
        s_in(3) <= to_signed(-30000, 32);  q_in(3) <= to_signed( -100, 32);
        pblk(3) <= mk_pb(10, -3000000, 5, -5, 500, 9, '1');
        valid_in <= '1';
        step;                 -- inputs registered; outputs appear next edge
        step;                 -- DUT + refs both present valid outputs now
        check_all;

        -- ---- vector 1 : change all lanes -----------------------------------
        s_in(0) <= to_signed(-100000, 32); q_in(0) <= to_signed(  -5, 32);
        pblk(0) <= mk_pb(11, 7000000, 1, 2, 80, 4, '0');
        s_in(1) <= to_signed( 999999, 32); q_in(1) <= to_signed( 199, 32);
        pblk(1) <= mk_pb(100, 123456, 9, -9, 150, 6, '0');
        s_in(2) <= to_signed(      0, 32); q_in(2) <= to_signed(   0, 32);
        pblk(2) <= mk_pb(0, 0, 0, 0, 10, 1, '0');
        s_in(3) <= to_signed(2147483647, 32); q_in(3) <= to_signed(3, 32);
        pblk(3) <= mk_pb(1000, 2000000000, 100, 100, 1000, 2, '0');
        step;
        step;
        check_all;

        -- ---- vector 2 : negative inventory beyond -max_inv on a lane -------
        s_in(0) <= to_signed( 12345, 32); q_in(0) <= to_signed(-500, 32);
        pblk(0) <= mk_pb(30, 1500000, -1, -1, 100, 5, '0');
        s_in(1) <= to_signed( 54321, 32); q_in(1) <= to_signed(  40, 32);
        pblk(1) <= mk_pb(60, -900000, 2, 3, 300, 8, '0');
        s_in(2) <= to_signed(-77777, 32); q_in(2) <= to_signed( -10, 32);
        pblk(2) <= mk_pb(15, 250000, 0, 7, 250, 3, '0');
        s_in(3) <= to_signed( 88888, 32); q_in(3) <= to_signed(  88, 32);
        pblk(3) <= mk_pb(45, 333333, -4, 4, 200, 6, '0');
        step;
        step;
        check_all;

        -- keep pipeline flowing a couple more cycles then stop valid
        valid_in <= '0';
        step; step;
        check_all;

        if errors > 0 then
            report "PASS (L2 tester's test): injected bug CAUGHT -- "
                 & integer'image(errors)
                 & " lane/cycle mismatches vs single-lane reference, so the L1 "
                 & "pricing_engine_multi parity test is load-bearing (not vacuous)"
                 severity note;
        else
            report "FAIL (L2): injected bug NOT caught -- the multi-lane parity "
                 & "test proves nothing!" severity failure;
        end if;

        done <= true;
        wait;
    end process;

end architecture;
