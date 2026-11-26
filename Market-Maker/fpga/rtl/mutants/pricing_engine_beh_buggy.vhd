-- MMX — fpga/rtl/pricing_engine_beh.vhd
-- Behavioral pricing core (spec §4.4): a bit-for-bit VHDL mirror of
-- mmx::compute_quote() in host/pricing/pricing_engine.h. Lets the Phase-B data
-- plane simulate without the Vitis HLS IP while preserving the host/FPGA parity
-- contract. Integer-only, Q1.31 inventory skew, signed 64-bit intermediates,
-- saturate to int32 (mirror of sat_i32). Single 250 MHz clock, sync reset.
--
-- Reference math (host/pricing/pricing_engine.h):
--   cap  = (int64)max_inv
--   kill = kill_switch | (q >  cap) | (q < -cap)
--   qc   = clamp(q, -cap, cap)
--   prod = qc * inv_skew_q31           (signed 32x32 -> 64)
--   skew = (prod + 2^30) >> 31         (round-to-nearest, arithmetic shift)
--   r    = s - skew
--   bid  = sat_i32(r - half_spread - bid_skew)
--   ask  = sat_i32(r + half_spread + ask_skew)
--   qty  = base_qty ;  cancel_only = kill

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity pricing_engine_beh_buggy is port( clk, rst : in std_logic;
  s_in : in tick_t; q_in : in tick_t; valid_in : in std_logic; pblk : in param_block_t;
  quote_out : out quote_t; quote_valid : out std_logic ); end entity;

architecture beh of pricing_engine_beh_buggy is

    -- int32 saturation bounds. -2^31 is built as (-(2^31-1) - 1) because the
    -- VHDL integer literal -2147483648 may exceed the host integer range.
    constant INT32_MAX_64 : signed(63 downto 0) := to_signed(2147483647, 64);
    constant INT32_MIN_64 : signed(63 downto 0) := to_signed(-2147483647, 64) - 1;
    constant INT32_MAX_32 : tick_t              := to_signed(2147483647, TICK_W);
    constant INT32_MIN_32 : tick_t              := to_signed(-2147483647, TICK_W) - 1;
    -- round-to-nearest pre-add: 2^30.
    constant ROUND_2P30   : signed(63 downto 0) := to_signed(1073741824, 64);

    -- Saturate an int64 to the int32 range; returns tick_t (signed 32).
    function sat_i32(v : signed(63 downto 0)) return tick_t is
    begin
        if    v > INT32_MAX_64 then return INT32_MAX_32;
        elsif v < INT32_MIN_64 then return INT32_MIN_32;
        else  return resize(v, TICK_W);
        end if;
    end function;

begin

    process(clk)
        variable cap_s   : signed(63 downto 0);
        variable q64     : signed(63 downto 0);
        variable qc      : signed(63 downto 0);
        variable kill    : std_logic;
        variable prod128 : signed(127 downto 0);
        variable prod    : signed(63 downto 0);
        variable skew    : signed(63 downto 0);
        variable r       : signed(63 downto 0);
        variable bid_v   : signed(63 downto 0);
        variable ask_v   : signed(63 downto 0);
    begin
        if rising_edge(clk) then
            if rst = '1' then
                quote_out   <= (bid => (others => '0'), ask => (others => '0'),
                                qty => (others => '0'), cancel_only => '0');
                quote_valid <= '0';
            else
                quote_valid <= valid_in;

                -- cap = (int64)max_inv  (unsigned 32 -> positive int64)
                cap_s := signed(resize(pblk.max_inv, 64));
                q64   := resize(q_in, 64);

                -- kill = kill_switch | (q > cap) | (q < -cap)
                if (q64 > cap_s) or (q64 < -cap_s) then
                    kill := pblk.kill_switch or '1';
                else
                    kill := pblk.kill_switch;
                end if;

                -- qc = clamp(q, -cap, cap) BEFORE the skew multiply
                qc := q64;
                if    qc >  cap_s then qc :=  cap_s;
                elsif qc < -cap_s then qc := -cap_s;
                end if;

                -- prod = qc * inv_skew_q31  (signed 32x32->64; full product fits)
                prod128 := qc * resize(pblk.inv_skew_q31, 64);
                prod    := resize(prod128, 64);

                -- skew = (prod + 2^30) >> 31  (arithmetic shift, round-to-nearest)
                skew := shift_right(prod, 31);  -- L2 INJECTED BUG: dropped round-to-nearest (+2^30)

                -- r = s - skew
                r := resize(s_in, 64) - skew;

                -- bid/ask formed in int64, saturated to int32
                bid_v := r - resize(pblk.half_spread, 64) - resize(pblk.bid_skew, 64);
                ask_v := r + resize(pblk.half_spread, 64) + resize(pblk.ask_skew, 64);

                quote_out.bid         <= sat_i32(bid_v);
                quote_out.ask         <= sat_i32(ask_v);
                quote_out.qty         <= pblk.base_qty;
                quote_out.cancel_only <= kill;
            end if;
        end if;
    end process;

end architecture;
