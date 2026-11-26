-- MMX — fpga/cl_dataplane/pricing_engine_multi_flat.vhd
-- Thin FLATTENING wrapper around pricing_engine_multi so it can be instantiated
-- from SystemVerilog (cl_multiasset.sv / cl_axil_reg_access). Vivado mixed-
-- language cannot connect SV to VHDL record/array ports (tick_vec_t / pb_vec_t /
-- quote_vec_t from work.mmx_multi_pkg), so this wrapper presents FLAT
-- std_logic_vector ports, rebuilds the per-lane records/arrays, and instantiates
-- pricing_engine_multi BY NAME. No added latency, no logic — pure packing.
--
-- Lane i occupies bits (i*W+W-1 downto i*W) of each flat bus (W=32 or 16).
-- ParamBlock fields not used by the pricing core (regime_id, requote_ticks,
-- epoch_ns) are tied to defaults; valid is forced '1' so the core prices.
--
-- Requires (compile first, in order): mmx_pkg, mmx_multi_pkg, pricing_engine_beh,
-- pricing_engine_multi.  VHDL-2008.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;
use work.mmx_multi_pkg.all;

entity pricing_engine_multi_flat is
    generic ( N : natural := 4 );
    port (
        clk, rst       : in  std_logic;
        valid_in       : in  std_logic;
        -- flattened per-lane inputs (lane i in bits i*W .. i*W+W-1)
        s_in           : in  std_logic_vector(N*32-1 downto 0);
        q_in           : in  std_logic_vector(N*32-1 downto 0);
        pb_inv_skew    : in  std_logic_vector(N*32-1 downto 0);
        pb_half_spread : in  std_logic_vector(N*32-1 downto 0);
        pb_max_inv     : in  std_logic_vector(N*32-1 downto 0);
        pb_base_qty    : in  std_logic_vector(N*32-1 downto 0);
        pb_bid_skew    : in  std_logic_vector(N*16-1 downto 0);
        pb_ask_skew    : in  std_logic_vector(N*16-1 downto 0);
        pb_kill        : in  std_logic_vector(N-1 downto 0);
        -- flattened per-lane outputs
        bid            : out std_logic_vector(N*32-1 downto 0);
        ask            : out std_logic_vector(N*32-1 downto 0);
        qty            : out std_logic_vector(N*32-1 downto 0);
        cancel         : out std_logic_vector(N-1 downto 0);
        quote_valid    : out std_logic
    );
end entity;

architecture rtl of pricing_engine_multi_flat is
    signal s_vec  : tick_vec_t(0 to N-1);
    signal q_vec  : tick_vec_t(0 to N-1);
    signal pb_vec : pb_vec_t(0 to N-1);
    signal qo_vec : quote_vec_t(0 to N-1);
begin

    gen_pack : for i in 0 to N-1 generate
        -- unpack flat inputs into the per-lane records/arrays
        s_vec(i) <= signed(s_in(i*32+31 downto i*32));
        q_vec(i) <= signed(q_in(i*32+31 downto i*32));

        pb_vec(i).inv_skew_q31  <= signed(pb_inv_skew   (i*32+31 downto i*32));
        pb_vec(i).half_spread   <= signed(pb_half_spread(i*32+31 downto i*32));
        pb_vec(i).max_inv       <= unsigned(pb_max_inv  (i*32+31 downto i*32));
        pb_vec(i).base_qty      <= unsigned(pb_base_qty (i*32+31 downto i*32));
        pb_vec(i).bid_skew      <= signed(pb_bid_skew   (i*16+15 downto i*16));
        pb_vec(i).ask_skew      <= signed(pb_ask_skew   (i*16+15 downto i*16));
        pb_vec(i).kill_switch   <= pb_kill(i);
        -- fields the pricing core ignores -> safe defaults
        pb_vec(i).regime_id     <= (others => '0');
        pb_vec(i).requote_ticks <= (others => '0');
        pb_vec(i).epoch_ns      <= (others => '0');
        pb_vec(i).valid         <= '1';

        -- repack per-lane quote outputs into flat buses
        bid(i*32+31 downto i*32) <= std_logic_vector(qo_vec(i).bid);
        ask(i*32+31 downto i*32) <= std_logic_vector(qo_vec(i).ask);
        qty(i*32+31 downto i*32) <= std_logic_vector(qo_vec(i).qty);
        cancel(i)                <= qo_vec(i).cancel_only;
    end generate;

    u_core : entity work.pricing_engine_multi
        generic map ( N => N )
        port map (
            clk         => clk,
            rst         => rst,
            s_in        => s_vec,
            q_in        => q_vec,
            valid_in    => valid_in,
            pblk        => pb_vec,
            quote_out   => qo_vec,
            quote_valid => quote_valid
        );

end architecture;
