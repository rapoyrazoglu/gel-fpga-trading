-- MMX — fpga/rtl/tb_vol_ewma.vhd
-- Self-checking GHDL testbench for vol_ewma.
-- Scenario: a CALM sequence (tiny tick deltas) followed by a VOLATILE burst
-- (large mid jumps). Asserts spread_add and vol_out rise after the burst.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity tb_vol_ewma is
end entity tb_vol_ewma;

architecture sim of tb_vol_ewma is

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

    dut : entity work.vol_ewma
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

        variable base : integer;
    begin
        -- synchronous reset
        rst      <= '1';
        valid_in <= '0';
        wait until rising_edge(clk);
        wait until rising_edge(clk);
        rst <= '0';
        wait until rising_edge(clk);

        -- ------------------------------------------------------------------
        -- CALM phase: mid drifts by +/-1 tick around 100000. Small squared
        -- deltas -> EWMA stays tiny -> spread_add ~ 0.
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
        wait until rising_edge(clk);   -- let last result register out
        wait until rising_edge(clk);

        spread_calm <= spread_add;
        vol_calm    <= vol_out;
        wait until rising_edge(clk);

        report "CALM  spread_add=" & integer'image(to_integer(spread_add)) &
               "  vol_out(hi)=" & integer'image(to_integer(vol_out(46 downto 16)));

        -- ------------------------------------------------------------------
        -- VOLATILE phase: mid whipsaws by ~1e6 ticks every update. Large
        -- squared deltas -> EWMA climbs -> spread_add clamps up.
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
        -- Checks
        -- ------------------------------------------------------------------
        assert vol_vol > vol_calm
            report "FAIL: EWMA vol did not rise after volatile burst"
            severity failure;

        assert spread_vol > spread_calm
            report "FAIL: spread_add did not rise after volatile burst"
            severity failure;

        assert spread_vol > 0
            report "FAIL: spread_add is zero after volatile burst"
            severity failure;

        assert spread_calm <= spread_vol
            report "FAIL: calm spread exceeded volatile spread"
            severity failure;

        report "PASS: vol_ewma calm->volatile spread widened  (calm=" &
               integer'image(to_integer(spread_calm)) & " vol=" &
               integer'image(to_integer(spread_vol)) & ")"
            severity note;

        done <= true;
        wait;
    end process;

end architecture sim;
