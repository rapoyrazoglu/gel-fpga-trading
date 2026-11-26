-- MMX — fpga/rtl/pricing_engine_multi.vhd
-- Multi-asset parallel pricing wrapper (spec §4.4, multi-symbol extension).
-- Instantiates the behavioral pricing core pricing_engine_beh once per asset
-- lane, all N lanes priced in the SAME cycle from a shared valid_in strobe.
-- Each lane has its own price s_in(i), inventory q_in(i) and ParamBlock pblk(i),
-- and drives its own quote_out(i). Outputs are registered inside each core, so
-- this wrapper is purely structural (no added combinational latency, no latches).
--
-- All lanes share clk/rst/valid_in, therefore all quote_valid strobes are
-- identical; the aggregate quote_valid is taken from lane 0.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;
use work.mmx_multi_pkg.all;

entity pricing_engine_multi is
    generic ( N : natural := 4 );
    port (
        clk, rst    : in  std_logic;
        s_in        : in  tick_vec_t(0 to N-1);
        q_in        : in  tick_vec_t(0 to N-1);
        valid_in    : in  std_logic;
        pblk        : in  pb_vec_t(0 to N-1);
        quote_out   : out quote_vec_t(0 to N-1);
        quote_valid : out std_logic
    );
end entity;

architecture rtl of pricing_engine_multi is
    -- Per-lane valid strobes from the cores; all identical (shared valid_in).
    signal lane_valid : std_logic_vector(0 to N-1);
begin

    gen_lanes : for i in 0 to N-1 generate
        u_lane : entity work.pricing_engine_beh
            port map (
                clk         => clk,
                rst         => rst,
                s_in        => s_in(i),
                q_in        => q_in(i),
                valid_in    => valid_in,
                pblk        => pblk(i),
                quote_out   => quote_out(i),
                quote_valid => lane_valid(i)
            );
    end generate;

    -- All lanes strobe together; expose lane 0's registered valid.
    quote_valid <= lane_valid(0);

end architecture;
