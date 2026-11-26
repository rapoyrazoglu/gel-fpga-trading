-- MMX — fpga/rtl/tb_obi_skew_l2.vhd
-- L2 "tester's test" for obi_skew: instantiates obi_skew_buggy (Q1.15 de-scale
-- shift off by one: sra 14 instead of sra 15) and runs the same L1-style
-- skew_adj magnitude check that tb_obi_skew applies. It reports PASS when the
-- injected bug is DETECTED (buggy skew_adj /= expected), proving the L1
-- skew_adj value assertion is load-bearing and not vacuous.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity tb_obi_skew_l2 is
end entity;

architecture sim of tb_obi_skew_l2 is

    constant CLAMP_MAX : positive := 64;

    signal clk        : std_logic := '0';
    signal rst        : std_logic := '1';
    signal imbalance  : signed(15 downto 0) := (others => '0');
    signal obi_gain   : signed(15 downto 0) := (others => '0');
    signal valid_in   : std_logic := '0';
    signal skew_adj   : tick_t;
    signal skew_valid : std_logic;

    signal done       : boolean := false;
    signal mismatches : natural := 0;

    constant TCLK : time := 4 ns;  -- 250 MHz

begin

    dut : entity work.obi_skew_buggy
        generic map (CLAMP_MAX => CLAMP_MAX)
        port map (
            clk        => clk,
            rst        => rst,
            imbalance  => imbalance,
            obi_gain   => obi_gain,
            valid_in   => valid_in,
            skew_adj   => skew_adj,
            skew_valid => skew_valid
        );

    -- free-running clock
    clk_gen : process
    begin
        while not done loop
            clk <= '0'; wait for TCLK/2;
            clk <= '1'; wait for TCLK/2;
        end loop;
        wait;
    end process;

    stim : process
        -- Same L1-style stimulus/sample as tb_obi_skew: drive one input, sample
        -- the registered result 1 cycle later, and compare against the GOLDEN
        -- expected value. On the buggy DUT the value must differ -> bug caught.
        procedure check(imb  : integer;
                        gain : integer;
                        exp  : integer) is
        begin
            imbalance <= to_signed(imb,  16);
            obi_gain  <= to_signed(gain, 16);
            valid_in  <= '1';
            wait until rising_edge(clk);   -- inputs+valid latched; output regs
            wait for TCLK/4;               -- settle past clock edge
            if to_integer(skew_adj) /= exp then
                mismatches <= mismatches + 1;
            end if;
        end procedure;
    begin
        -- reset
        rst <= '1';
        wait until rising_edge(clk);
        wait until rising_edge(clk);
        rst <= '0';
        wait until rising_edge(clk);

        -- Same golden vectors as tb_obi_skew (expected = GOOD obi_skew output).
        check(0, 1000, 0);
        check(26810, 50, 40);
        check(-26810, 50, -41);
        check(32767, 100, CLAMP_MAX);
        check(-32768, 100, -CLAMP_MAX);
        check(32767, 1, 0);
        check(16384, 32, 16);

        wait until rising_edge(clk);

        if mismatches > 0 then
            report "PASS (L2 tester's test): injected bug CAUGHT -- obi_skew_buggy "
                 & "skew_adj deviated from the golden value on "
                 & integer'image(mismatches) & " vector(s), so the obi_skew "
                 & "skew_adj magnitude check is load-bearing" severity note;
        else
            report "FAIL (L2): injected bug NOT caught -- the obi_skew skew_adj "
                 & "check proves nothing!" severity failure;
        end if;

        done <= true;
        wait;
    end process;

end architecture sim;
