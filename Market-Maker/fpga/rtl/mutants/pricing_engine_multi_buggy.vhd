-- MMX — fpga/rtl/pricing_engine_multi_buggy.vhd
-- L2 TESTER'S-TEST COPY of pricing_engine_multi with exactly ONE injected bug.
-- Verbatim copy of pricing_engine_multi (entity renamed to
-- pricing_engine_multi_buggy) EXCEPT the per-lane input port map swaps the
-- s_in and q_in fields (a flipped-field bug). The L2 testbench asserts the
-- L1-style per-lane parity checks CATCH this bug.
--
-- Multi-asset parallel pricing wrapper (spec §4.4, multi-symbol extension).
-- Instantiates the behavioral pricing core pricing_engine_beh once per asset
-- lane, all N lanes priced in the SAME cycle from a shared valid_in strobe.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;
use work.mmx_multi_pkg.all;

entity pricing_engine_multi_buggy is
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

architecture rtl of pricing_engine_multi_buggy is
    -- Per-lane valid strobes from the cores; all identical (shared valid_in).
    signal lane_valid : std_logic_vector(0 to N-1);
begin

    gen_lanes : for i in 0 to N-1 generate
        u_lane : entity work.pricing_engine_beh
            port map (
                clk         => clk,
                rst         => rst,
                s_in        => q_in(i),   -- L2 INJECTED BUG: flipped field (s_in fed from q_in)
                q_in        => s_in(i),   -- L2 INJECTED BUG: flipped field (q_in fed from s_in)
                valid_in    => valid_in,
                pblk        => pblk(i),
                quote_out   => quote_out(i),
                quote_valid => lane_valid(i)
            );
    end generate;

    -- All lanes strobe together; expose lane 0's registered valid.
    quote_valid <= lane_valid(0);

end architecture;
