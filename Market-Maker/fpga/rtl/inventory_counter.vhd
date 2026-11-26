-- MMX — fpga/rtl/inventory_counter.vhd
-- Phase B data plane, spec MMX_SPECIFICATION.md §4.7 (fill-driven inventory).
--
-- Maintains the signed running inventory q from OUR OWN fills:
--   fill_in.side = BID  -> our resting bid was hit, we BOUGHT  -> q += qty
--   fill_in.side = ASK  -> our resting ask was lifted, we SOLD -> q -= qty
-- The running total is saturated into [-max_inv, +max_inv] every update using
-- the shared sat_add() helper (max_inv cast to tick_t). This module is the
-- SINGLE WRITER of q. Outputs:
--   q_out     : signed inventory, driven continuously (current register value)
--   q_valid   : single-cycle pulse asserted the cycle q is updated by a fill
--   q_cpu_reg : std_logic_vector mirror of q for a CPU AXI-Lite read
-- Reset clears q to 0. 250 MHz single clock, synchronous active-high reset.
--
-- VHDL-2008. Mirrors host inventory math (inventory sign convention in
-- host/pricing/pricing_engine.h: positive q = long, negative q = short).

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity inventory_counter is port( clk, rst : in std_logic;
  fill_in : in fill_t; max_inv : in unsigned(31 downto 0);
  q_out : out tick_t; q_valid : out std_logic; q_cpu_reg : out std_logic_vector(31 downto 0) ); end entity;

architecture rtl of inventory_counter is

    -- single architectural state: the running signed inventory.
    signal q_reg     : tick_t := (others => '0');
    signal q_vld_reg : std_logic := '0';

    -- tick_t representable bounds, used to saturate (not truncate) a wide delta.
    constant TICK_MAX : tick_t := to_signed(2147483647, TICK_W);  --  2^31 - 1
    constant TICK_MIN : tick_t := -TICK_MAX - 1;                  -- -2^31

begin

    -- ----------------------------------------------------------------------
    -- Continuous output drives from the registers (no combinational paths to
    -- the outside; everything is registered for timing closure at 250 MHz).
    -- ----------------------------------------------------------------------
    q_out     <= q_reg;
    q_valid   <= q_vld_reg;
    q_cpu_reg <= std_logic_vector(q_reg);

    process(clk)
        -- saturation bounds from the (live) cap. max_inv is a non-negative
        -- magnitude; clamp window is symmetric [-max_inv, +max_inv].
        variable hi    : tick_t;
        variable lo    : tick_t;
        -- signed-extended fill quantity (33-bit non-negative) so we never lose
        -- the sign when forming +qty / -qty deltas.
        variable q_ext : signed(TICK_W downto 0);
        variable d_wide: signed(TICK_W downto 0);
        variable delta : tick_t;
    begin
        if rising_edge(clk) then
            if rst = '1' then
                q_reg     <= (others => '0');   -- spec §4.7: reset q = 0
                q_vld_reg <= '0';
            else
                -- default: no update this cycle (no latch — assigned every path)
                q_vld_reg <= '0';

                if fill_in.valid = '1' then
                    -- cap window: max_inv cast to tick_t, mirrored as [-cap,+cap]
                    hi := signed(resize(max_inv, TICK_W));
                    lo := -hi;

                    -- qty -> non-negative 33-bit signed, then 32-bit delta
                    q_ext := signed(resize(fill_in.qty, TICK_W+1));

                    if fill_in.side = BID then
                        d_wide := q_ext;                         -- bought: +qty
                    else
                        d_wide := -q_ext;                        -- sold:   -qty
                    end if;

                    -- Narrow the 33-bit signed delta into tick_t by SATURATION,
                    -- never by a truncating resize(): for qty >= 2^31 a plain
                    -- resize drops the sign bit and would flip a large sell into
                    -- a phantom buy. sat_add then re-clamps into [lo, hi].
                    if    d_wide > resize(TICK_MAX, TICK_W+1) then
                        delta := TICK_MAX;
                    elsif d_wide < resize(TICK_MIN, TICK_W+1) then
                        delta := TICK_MIN;
                    else
                        delta := resize(d_wide, TICK_W);
                    end if;

                    -- single writer, saturating into [lo, hi]
                    q_reg     <= sat_add(q_reg, delta, lo, hi);
                    q_vld_reg <= '1';
                end if;
            end if;
        end if;
    end process;

end architecture rtl;
