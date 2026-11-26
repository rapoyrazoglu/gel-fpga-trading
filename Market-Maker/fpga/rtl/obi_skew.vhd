-- MMX — fpga/rtl/obi_skew.vhd
-- Order-Book Imbalance (OBI) skew (spec §4.x quote skewing).
--
-- Converts a Q1.15 order-book imbalance into a tick-domain skew adjustment that
-- is later ADDED to the pricing_engine reservation-price skew (see r = s - skew
-- in pricing_engine_beh.vhd). Positive imbalance (bidQ > askQ, more buyers) ->
-- positive skew_adj, which shifts quotes UP.
--
--   imbalance : Q1.15 signed, = (bidQ-askQ)/(bidQ+askQ)   [book_out_t.imbalance]
--   obi_gain  : Q?.? tunable, "ticks per unit imbalance"  (treated as integer
--               ticks: at |imbalance|=1.0 (Q1.15 = +/-32768) the raw skew is
--               obi_gain ticks, since (imbalance*obi_gain) >> 15 = obi_gain).
--   skew_adj  = clamp( (imbalance * obi_gain) sra 15, -CLAMP_MAX, +CLAMP_MAX )
--
-- Integer-only, numeric_std only. Single 250 MHz clock, synchronous active-high
-- reset. Registered outputs (1-cycle latency). No latches, no wait/after.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity obi_skew is
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
end entity obi_skew;

architecture rtl of obi_skew is

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
                scaled := shift_right(prod, 15);

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
