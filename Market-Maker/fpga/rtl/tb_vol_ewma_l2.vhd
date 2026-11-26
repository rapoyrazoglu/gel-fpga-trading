-- MMX — fpga/rtl/tb_vol_ewma_l2.vhd
-- L2 tester's-test for vol_ewma.
--
-- Instantiates the deliberately BUGGY copy (vol_ewma_buggy, which drops the
-- squared-delta input term from the EWMA recurrence) and drives it EXACTLY the
-- way tb_vol_ewma drives the good vol_ewma: a CALM phase of +/-1 tick jitter
-- followed by a VOLATILE burst of ~1e6 tick whipsaws. It then applies the same
-- L1-style checks (vol/spread must rise after the burst).
--
-- Semantics are inverted vs. L1: here PASS means "the L1 checks CAUGHT the bug".
-- If the buggy DUT still satisfied every L1 check, the L1 test would be vacuous
-- and this L2 test FAILS.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity tb_vol_ewma_l2 is
end entity tb_vol_ewma_l2;

architecture sim of tb_vol_ewma_l2 is

    constant TCLK : time := 4 ns;

    signal clk        : std_logic := '0';
    signal rst        : std_logic := '1';
    signal mid        : tick_t := (others => '0');
    signal valid_in   : std_logic := '0';
    signal k_shift    : integer range 1 to 15 := 4;
    signal gain       : unsigned(15 downto 0) := to_unsigned(65535, 16);
    signal spread_add : qty_t;
    signal vol_out    : unsigned(63 downto 0);
    signal valid      : std_logic;

    signal done : boolean := false;

    signal spread_calm : unsigned(31 downto 0) := (others => '0');
    signal vol_calm    : unsigned(63 downto 0) := (others => '0');
    signal spread_vol  : unsigned(31 downto 0) := (others => '0');
    signal vol_vol     : unsigned(63 downto 0) := (others => '0');

begin

    -- clock
    clk <= '0' when done else not clk after TCLK/2;

    -- L2: drive the BUGGY module
    dut : entity work.vol_ewma_buggy
        generic map ( SPREAD_MAX => 65535 )
        port map (
            clk        => clk,
            rst        => rst,
            mid        => mid,
            valid_in   => valid_in,
            k_shift    => k_shift,
            gain       => gain,
            spread_add => spread_add,
            vol_out    => vol_out,
            valid      => valid
        );

    stim : process
        -- apply one tick and advance a clock edge
        procedure tick(m : integer) is
        begin
            mid      <= to_signed(m, TICK_W);
            valid_in <= '1';
            wait until rising_edge(clk);
        end procedure;

        variable base       : integer;
        variable bug_caught : boolean := false;
    begin
        -- synchronous reset
        rst      <= '1';
        valid_in <= '0';
        wait until rising_edge(clk);
        wait until rising_edge(clk);
        rst <= '0';
        wait until rising_edge(clk);

        -- ------------------------------------------------------------------
        -- CALM phase (identical stimulus to tb_vol_ewma).
        -- ------------------------------------------------------------------
        base := 100000;
        for i in 0 to 63 loop
            if (i mod 2) = 0 then
                tick(base + 1);
            else
                tick(base);
            end if;
        end loop;

        valid_in <= '0';
        wait until rising_edge(clk);
        wait until rising_edge(clk);

        spread_calm <= spread_add;
        vol_calm    <= vol_out;
        wait until rising_edge(clk);

        report "CALM  spread_add=" & integer'image(to_integer(spread_add)) &
               "  vol_out(hi)=" & integer'image(to_integer(vol_out(46 downto 16)));

        -- ------------------------------------------------------------------
        -- VOLATILE phase (identical stimulus to tb_vol_ewma).
        -- ------------------------------------------------------------------
        for i in 0 to 63 loop
            if (i mod 2) = 0 then
                tick(100000 + 1000000);
            else
                tick(100000 - 1000000);
            end if;
        end loop;

        valid_in <= '0';
        wait until rising_edge(clk);
        wait until rising_edge(clk);

        spread_vol <= spread_add;
        vol_vol    <= vol_out;
        wait until rising_edge(clk);

        report "VOL   spread_add=" & integer'image(to_integer(spread_add)) &
               "  vol_out(hi)=" & integer'image(to_integer(vol_out(46 downto 16)));

        -- ------------------------------------------------------------------
        -- Same L1-style checks, but as booleans so we can detect whether the
        -- injected bug broke them (that is the PASS condition for L2).
        -- The good module makes ALL of these true; the buggy module (EWMA that
        -- never rises with volatility) must violate at least one.
        -- ------------------------------------------------------------------
        if not (vol_vol > vol_calm) then
            bug_caught := true;
            report "L1-check tripped: EWMA vol did not rise after volatile burst"
                severity note;
        end if;
        if not (spread_vol > spread_calm) then
            bug_caught := true;
            report "L1-check tripped: spread_add did not rise after volatile burst"
                severity note;
        end if;
        if not (spread_vol > 0) then
            bug_caught := true;
            report "L1-check tripped: spread_add is zero after volatile burst"
                severity note;
        end if;

        -- ------------------------------------------------------------------
        -- L2 verdict
        -- ------------------------------------------------------------------
        if bug_caught then
            report "PASS (L2 tester's test): injected bug CAUGHT -- the vol_ewma "
                 & "calm->volatile checks flagged the dropped d2 term (calm vol(hi)="
                 & integer'image(to_integer(vol_calm(46 downto 16)))
                 & " vol vol(hi)="
                 & integer'image(to_integer(vol_vol(46 downto 16)))
                 & "), so the L1 test is load-bearing (not vacuous)"
                severity note;
        else
            report "FAIL (L2): injected bug NOT caught -- the L1 vol_ewma test "
                 & "proves nothing!"
                severity failure;
        end if;

        done <= true;
        wait;
    end process;

end architecture sim;
