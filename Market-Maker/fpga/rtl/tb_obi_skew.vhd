-- MMX — fpga/rtl/tb_obi_skew.vhd
-- Self-checking GHDL testbench for obi_skew. Drives (imbalance, gain) pairs
-- through the 1-cycle registered pipeline and checks skew_adj / skew_valid,
-- including a 10:1 book imbalance and both clamp rails. Prints PASS on success.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity tb_obi_skew is
end entity;

architecture tb of tb_obi_skew is

    constant CLAMP_MAX : positive := 64;

    signal clk        : std_logic := '0';
    signal rst        : std_logic := '1';
    signal imbalance  : signed(15 downto 0) := (others => '0');
    signal obi_gain   : signed(15 downto 0) := (others => '0');
    signal valid_in   : std_logic := '0';
    signal skew_adj   : tick_t;
    signal skew_valid : std_logic;

    signal done       : boolean := false;
    signal errors     : natural := 0;

    constant TCLK : time := 4 ns;  -- 250 MHz

begin

    dut : entity work.obi_skew
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
        -- Drive one input, then sample the registered result 1 cycle later.
        procedure check(imb  : integer;
                        gain : integer;
                        exp  : integer) is
        begin
            imbalance <= to_signed(imb,  16);
            obi_gain  <= to_signed(gain, 16);
            valid_in  <= '1';
            wait until rising_edge(clk);   -- inputs+valid latched; output regs
                                           -- update on this same edge (1-cycle)
            wait for TCLK/4;               -- settle past clock edge
            assert skew_valid = '1'
                report "skew_valid low when expected high" severity error;
            if skew_valid /= '1' then errors <= errors + 1; end if;
            assert to_integer(skew_adj) = exp
                report "MISMATCH imb=" & integer'image(imb) &
                       " gain=" & integer'image(gain) &
                       " got="  & integer'image(to_integer(skew_adj)) &
                       " exp="  & integer'image(exp)
                severity error;
            if to_integer(skew_adj) /= exp then errors <= errors + 1; end if;
        end procedure;
    begin
        -- reset
        rst <= '1';
        wait until rising_edge(clk);
        wait until rising_edge(clk);
        rst <= '0';
        wait until rising_edge(clk);

        -- zero imbalance -> zero skew
        check(0, 1000, 0);

        -- 10:1 book (bidQ=10, askQ=1): imbalance = 9/11 = 0.8181 in Q1.15 = 26810.
        -- (26810 * 50) >> 15 = 1340500/32768 = 40 (floor).  Within clamp.
        check(26810, 50, 40);

        -- symmetric negative moderate: (-26810 * 50) >> 15 = floor(-40.9) = -41.
        check(-26810, 50, -41);

        -- positive clamp rail: (32767 * 100) >> 15 = 99 -> clamp to +64.
        check(32767, 100, CLAMP_MAX);

        -- negative clamp rail: (-32768 * 100) >> 15 = -100 -> clamp to -64.
        check(-32768, 100, -CLAMP_MAX);

        -- unit gain, positive: (32767 * 1) >> 15 = 0 (0.999 floors to 0).
        check(32767, 1, 0);

        -- small positive: (16384 * 32) >> 15 = 524288/32768 = 16.
        check(16384, 32, 16);

        -- check valid gating: drive valid_in='0' and confirm skew_valid drops.
        imbalance <= to_signed(26810, 16);
        obi_gain  <= to_signed(50, 16);
        valid_in  <= '0';
        wait until rising_edge(clk);
        wait until rising_edge(clk);
        wait for TCLK/4;
        assert skew_valid = '0'
            report "skew_valid high when valid_in was low" severity error;
        if skew_valid /= '0' then errors <= errors + 1; end if;

        -- report
        if errors = 0 then
            report "PASS: obi_skew all checks green" severity note;
        else
            report "FAIL: obi_skew had " & integer'image(errors) & " error(s)"
                severity failure;
        end if;

        done <= true;
        wait;
    end process;

end architecture;
