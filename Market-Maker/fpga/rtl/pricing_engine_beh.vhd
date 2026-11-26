-- MMX — fpga/rtl/pricing_engine_beh.vhd
-- Behavioral pricing core (spec §4.4): a bit-for-bit VHDL mirror of
-- mmx::compute_quote() in host/pricing/pricing_engine.h.
--
-- PIPELINED (5 stages) for 250 MHz timing closure in the full data plane. The
-- single-cycle version put clamp -> 32x32 multiply -> round/shift -> subtract ->
-- subtract -> saturate on one combinational path (~8 ns, -4 ns @250MHz). This
-- version splits that chain across five registered stages; the arithmetic is
-- IDENTICAL (same operations, same order, same rounding), so host<->FPGA parity
-- is preserved bit-for-bit. `valid_in` is delayed the same 5 stages to align
-- with `quote_out` — the downstream quote_decider consumes on `quote_valid`, so
-- the extra latency is transparent.
--
-- Reference math (host/pricing/pricing_engine.h):
--   cap  = (int64)max_inv ; kill = kill_switch | (q > cap) | (q < -cap)
--   qc   = clamp(q, -cap, cap) ; prod = qc * inv_skew_q31 (signed 32x32->64)
--   skew = (prod + 2^30) >> 31 ; r = s - skew
--   bid  = sat_i32(r - half_spread - bid_skew)
--   ask  = sat_i32(r + half_spread + ask_skew) ; qty = base_qty ; cancel = kill

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity pricing_engine_beh is port( clk, rst : in std_logic;
  s_in : in tick_t; q_in : in tick_t; valid_in : in std_logic; pblk : in param_block_t;
  quote_out : out quote_t; quote_valid : out std_logic;
  dbg_qc : out std_logic_vector(31 downto 0) := (others => '0') ); end entity;

architecture beh of pricing_engine_beh is

    constant INT32_MAX_64 : signed(63 downto 0) := to_signed(2147483647, 64);
    constant INT32_MIN_64 : signed(63 downto 0) := to_signed(-2147483647, 64) - 1;
    constant INT32_MAX_32 : tick_t              := to_signed(2147483647, TICK_W);
    constant INT32_MIN_32 : tick_t              := to_signed(-2147483647, TICK_W) - 1;
    constant ROUND_2P30   : signed(63 downto 0) := to_signed(1073741824, 64);

    function sat_i32(v : signed(63 downto 0)) return tick_t is
    begin
        if    v > INT32_MAX_64 then return INT32_MAX_32;
        elsif v < INT32_MIN_64 then return INT32_MIN_32;
        else  return resize(v, TICK_W);
        end if;
    end function;

    -- valid pipeline aligns with the 5 data stages (quote_out is stage 5).
    signal v1, v2, v3, v4 : std_logic := '0';

    -- Stage 1 (clamp + kill + latch resized params)
    -- qc is clamped to +/-max_inv (<=2^32-1 -> fits signed 34); inv_skew_q31 is
    -- 32-bit. Keeping the multiply operands NARROW (34x32, ~2-3 DSP) instead of
    -- 64x64 (which cost ~5ns / 10 CARRY8) is what closes stage-2 timing.
    signal qc_1   : signed(33 downto 0) := (others => '0');
    signal s_1    : signed(63 downto 0) := (others => '0');
    signal is_1   : signed(31 downto 0) := (others => '0');
    signal hs_1   : signed(63 downto 0) := (others => '0');
    signal bsk_1  : signed(63 downto 0) := (others => '0');
    signal ask_1  : signed(63 downto 0) := (others => '0');
    signal kill_1 : std_logic := '0';
    signal bq_1   : qty_t := (others => '0');

    -- Stage 2 (multiply)
    signal prod_2 : signed(63 downto 0) := (others => '0');
    signal s_2    : signed(63 downto 0) := (others => '0');
    signal hs_2   : signed(63 downto 0) := (others => '0');
    signal bsk_2  : signed(63 downto 0) := (others => '0');
    signal ask_2  : signed(63 downto 0) := (others => '0');
    signal kill_2 : std_logic := '0';
    signal bq_2   : qty_t := (others => '0');

    -- Stage 3 (skew + reservation price)
    signal r_3    : signed(63 downto 0) := (others => '0');
    signal hs_3   : signed(63 downto 0) := (others => '0');
    signal bsk_3  : signed(63 downto 0) := (others => '0');
    signal ask_3  : signed(63 downto 0) := (others => '0');
    signal kill_3 : std_logic := '0';
    signal bq_3   : qty_t := (others => '0');

    -- Stage 4 (raw bid/ask before saturation)
    signal bid_4  : signed(63 downto 0) := (others => '0');
    signal ask4_4 : signed(63 downto 0) := (others => '0');
    signal kill_4 : std_logic := '0';
    signal bq_4   : qty_t := (others => '0');

begin

    -- debug: expose the stage-1 clamped inventory actually fed to the skew multiply
    dbg_qc <= std_logic_vector(resize(qc_1, 32));

    process(clk)
        variable cap_s : signed(63 downto 0);
        variable q64   : signed(63 downto 0);
        variable qcl   : signed(63 downto 0);
        variable k     : std_logic;
    begin
        if rising_edge(clk) then
            if rst = '1' then
                v1 <= '0'; v2 <= '0'; v3 <= '0'; v4 <= '0';
                quote_out   <= (bid => (others => '0'), ask => (others => '0'),
                                qty => (others => '0'), cancel_only => '0');
                quote_valid <= '0';
            else
                -- ---- Stage 1: clamp inventory, compute kill, latch params ----
                cap_s := signed(resize(pblk.max_inv, 64));
                q64   := resize(q_in, 64);
                qcl   := q64;
                if    qcl >  cap_s then qcl :=  cap_s;
                elsif qcl < -cap_s then qcl := -cap_s;
                end if;
                if (q64 > cap_s) or (q64 < -cap_s) then k := pblk.kill_switch or '1';
                else k := pblk.kill_switch; end if;
                qc_1 <= resize(qcl, 34); kill_1 <= k; bq_1 <= pblk.base_qty;
                s_1  <= resize(s_in, 64);
                is_1 <= pblk.inv_skew_q31;
                hs_1 <= resize(pblk.half_spread, 64);
                bsk_1<= resize(pblk.bid_skew, 64);
                ask_1<= resize(pblk.ask_skew, 64);
                v1   <= valid_in;

                -- ---- Stage 2: skew multiply (DSP) ----
                prod_2 <= resize(qc_1 * is_1, 64);
                s_2<=s_1; hs_2<=hs_1; bsk_2<=bsk_1; ask_2<=ask_1; kill_2<=kill_1; bq_2<=bq_1;
                v2 <= v1;

                -- ---- Stage 3: round/shift -> skew, reservation price r ----
                r_3 <= s_2 - shift_right(prod_2 + ROUND_2P30, 31);
                hs_3<=hs_2; bsk_3<=bsk_2; ask_3<=ask_2; kill_3<=kill_2; bq_3<=bq_2;
                v3 <= v2;

                -- ---- Stage 4: raw bid/ask (subtracts/adds only) ----
                bid_4  <= r_3 - hs_3 - bsk_3;
                ask4_4 <= r_3 + hs_3 + ask_3;
                kill_4 <= kill_3; bq_4 <= bq_3;
                v4 <= v3;

                -- ---- Stage 5: saturate -> outputs ----
                quote_out.bid         <= sat_i32(bid_4);
                quote_out.ask         <= sat_i32(ask4_4);
                quote_out.qty         <= bq_4;
                quote_out.cancel_only <= kill_4;
                quote_valid           <= v4;
            end if;
        end if;
    end process;

end architecture;
