-- MMX — fpga/rtl/vol_ewma.vhd
-- On-FPGA realized-volatility estimator -> adaptive half-spread widener.
--
-- Per accepted tick (valid_in='1') the block tracks an exponentially-weighted
-- moving average (EWMA) of squared mid-price tick-deltas and turns that into an
-- extra half-spread (in ticks) that the quote path can add on top of the
-- ParamBlock half_spread when the market gets choppy.
--
-- Reference math (integer-only, register outputs, single 250 MHz clock):
--   d    = mid - prev_mid                       (signed)
--   d2   = d * d                                (>= 0, clamped to 64 bits)
--   ewma <= ewma - (ewma >> k_shift) + (d2 >> k_shift)   -- lambda = 1 - 2^-k
--   prod = ewma * gain                          (unsigned 64 x 16 -> 80)
--   spread_add = clamp( prod >> 16, 0, SPREAD_MAX )
--
-- The first accepted tick after reset only primes prev_mid (delta undefined),
-- emitting zero volatility so a cold start cannot inject a spurious huge jump.
--
-- Integration: mid comes from book_out_t.mid (order_book). spread_add feeds the
-- quote_decider / pricing path as an additive half-spread term (ticks, qty_t).
-- vol_out is exported for CPU telemetry / regime detection.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity vol_ewma is
    generic (
        SPREAD_MAX : natural := 65535        -- upper clamp for spread_add (ticks)
    );
    port (
        clk        : in  std_logic;
        rst        : in  std_logic;                       -- synchronous, active high
        mid        : in  tick_t;                          -- signed32 mid price
        valid_in   : in  std_logic;                       -- new tick strobe
        k_shift    : in  integer range 1 to 15;           -- EWMA decay, lambda=1-2^-k
        gain       : in  unsigned(15 downto 0);           -- vol->spread gain (Q?.0)
        spread_add : out qty_t;                           -- extra half-spread ticks, >=0
        vol_out    : out unsigned(63 downto 0);           -- EWMA of squared tick deltas
        valid      : out std_logic
    );
end entity vol_ewma;

architecture rtl of vol_ewma is

    -- registers
    signal prev_mid   : tick_t             := (others => '0');
    signal primed     : std_logic          := '0';
    signal ewma_r     : unsigned(63 downto 0) := (others => '0');
    signal spread_r   : qty_t              := (others => '0');
    signal vol_r      : unsigned(63 downto 0) := (others => '0');
    signal valid_r    : std_logic          := '0';

    constant SPREAD_MAX_U : unsigned(63 downto 0) :=
        to_unsigned(SPREAD_MAX, 64);

begin

    spread_add <= spread_r;
    vol_out    <= vol_r;
    valid      <= valid_r;

    process(clk)
        variable d        : signed(32 downto 0);   -- mid - prev_mid (33-bit)
        variable d2f      : signed(65 downto 0);   -- d*d, always >= 0
        variable d2u      : unsigned(63 downto 0); -- d2 clamped to 64 bits
        variable ewma_v   : unsigned(63 downto 0);
        variable ewma_nxt : unsigned(63 downto 0);
        variable prod     : unsigned(79 downto 0); -- ewma * gain
        variable prod_sh  : unsigned(79 downto 0); -- prod >> 16
    begin
        if rising_edge(clk) then
            if rst = '1' then
                prev_mid <= (others => '0');
                primed   <= '0';
                ewma_r   <= (others => '0');
                spread_r <= (others => '0');
                vol_r    <= (others => '0');
                valid_r  <= '0';
            else
                -- valid tracks the input strobe one cycle later
                valid_r <= valid_in;

                if valid_in = '1' then
                    if primed = '0' then
                        -- first tick: only latch prev_mid, emit zeros
                        prev_mid <= mid;
                        primed   <= '1';
                        ewma_r   <= (others => '0');
                        spread_r <= (others => '0');
                        vol_r    <= (others => '0');
                    else
                        -- d = mid - prev_mid
                        d := resize(mid, 33) - resize(prev_mid, 33);

                        -- d2 = d*d  (66-bit signed, non-negative)
                        d2f := d * d;

                        -- clamp squared delta into 64 bits (overflow -> saturate)
                        if d2f(65 downto 64) /= "00" then
                            d2u := (others => '1');
                        else
                            d2u := unsigned(d2f(63 downto 0));
                        end if;

                        -- ewma <= ewma - (ewma >> k) + (d2 >> k)
                        ewma_v   := ewma_r;
                        ewma_nxt := ewma_v
                                    - shift_right(ewma_v, k_shift)
                                    + shift_right(d2u, k_shift);

                        -- prod = ewma * gain ; spread = clamp(prod >> 16, 0, MAX)
                        prod    := ewma_nxt * gain;
                        prod_sh := shift_right(prod, 16);

                        if prod_sh > resize(SPREAD_MAX_U, 80) then
                            spread_r <= resize(SPREAD_MAX_U, QTY_W);
                        else
                            spread_r <= resize(prod_sh, QTY_W);
                        end if;

                        ewma_r   <= ewma_nxt;
                        vol_r    <= ewma_nxt;
                        prev_mid <= mid;
                    end if;
                end if;
            end if;
        end if;
    end process;

end architecture rtl;
