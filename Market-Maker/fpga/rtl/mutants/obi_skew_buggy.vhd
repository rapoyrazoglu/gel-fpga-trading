-- MMX — fpga/rtl/obi_skew_buggy.vhd
-- DELIBERATELY BUGGY copy of obi_skew for the L2 "tester's test".
-- INJECTED BUG: the Q1.15 de-scale shift is off by one (sra 14 instead of
-- sra 15), so every skew_adj comes out ~2x too large. The L1 magnitude check
-- must catch this. Everything else is verbatim from obi_skew.vhd.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity obi_skew_buggy is
    generic (
        -- Maximum absolute skew contribution, in ticks.
        CLAMP_MAX : positive := 64
    );
    port (
        clk        : in  std_logic;
        rst        : in  std_logic;                     -- sync, active-high
        imbalance  : in  signed(15 downto 0);           -- Q1.15
        obi_gain   : in  signed(15 downto 0);           -- ticks per unit imbalance
        valid_in   : in  std_logic;
        skew_adj   : out tick_t;                        -- signed(31 downto 0)
        skew_valid : out std_logic
    );
end entity obi_skew_buggy;

architecture rtl of obi_skew_buggy is

    -- 16 x 16 -> 32 bit signed product; >>15 leaves it comfortably inside int32.
    constant CLAMP_HI : tick_t := to_signed( CLAMP_MAX, TICK_W);
    constant CLAMP_LO : tick_t := to_signed(-CLAMP_MAX, TICK_W);

begin

    process(clk)
        variable prod   : signed(31 downto 0);   -- imbalance * obi_gain
        variable scaled : signed(31 downto 0);   -- (prod) sra 15
    begin
        if rising_edge(clk) then
            if rst = '1' then
                skew_adj   <= (others => '0');
                skew_valid <= '0';
            else
                skew_valid <= valid_in;

                -- signed 16x16 -> 32 exact product
                prod   := imbalance * obi_gain;

                -- arithmetic shift right by 15 (undo the Q1.15 scale)
                -- INJECTED BUG: off-by-one shift amount (14 instead of 15).
                scaled := shift_right(prod, 14);

                -- clamp into [-CLAMP_MAX, +CLAMP_MAX] ticks
                if    scaled > CLAMP_HI then
                    skew_adj <= CLAMP_HI;
                elsif scaled < CLAMP_LO then
                    skew_adj <= CLAMP_LO;
                else
                    skew_adj <= resize(scaled, TICK_W);
                end if;
            end if;
        end if;
    end process;

end architecture rtl;
