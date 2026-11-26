-- MMX — fpga/rtl/param_block_loader.vhd
-- ParamBlock DMA/AXI-Lite receiver + HMAC gate (spec §4.8; §3.2, §7.2, §7.3).
--
-- The CPU writes the 64-byte ParamBlock as 16 x 32-bit words through a simple
-- write port (s_wr_en, s_wr_addr 0..15, s_wr_data) into a staging buffer. On
-- s_commit the staged words are snapshotted and the signed 48-byte region
-- (words 0..11 = 384 bits) plus the 16-byte tag (words 12..15 = 128 bits) are
-- handed to an hmac_verify instance.
--
-- On hmac done:
--   * ok='0'                        -> load_err="01" (HMAC fail), force kill.
--   * stale: now>=epoch and
--            now-epoch > STALENESS_NS -> load_err="10" (stale),   force kill.
--   * else  -> atomically swap pblk_out to the staged block decoded per the
--             host/ipc/param_block.h byte offsets, valid='1', load_err="00".
--
-- Default pblk_out = PARAM_BLOCK_INIT (kill_switch='1', valid='0') until the
-- first good load. The active block is also continuously re-checked: if it ages
-- past STALENESS_NS the loader auto-kills it (§7.3, 500 ms).
--
-- Word -> field map (little-endian within each 32-bit word):
--   w0  bytes  0..3   inv_skew_q31  (int32)
--   w1  bytes  4..7   half_spread   (int32)
--   w2  bytes  8..11  max_inv       (uint32)
--   w3  bytes 12..15  base_qty      (uint32)
--   w4  byte  16      regime_id     (uint8 , bits  7..0)
--       byte  17      kill_switch   (uint8 , bits 15..8 ; nonzero => kill)
--       bytes 18..19  bid_skew      (int16 , bits 31..16)
--   w5  bytes 20..21  ask_skew      (int16 , bits 15..0)
--       bytes 22..23  requote_ticks (uint16, bits 31..16)
--   w6  bytes 24..27  epoch_ns      low  32
--   w7  bytes 28..31  epoch_ns      high 32
--   w8..w11 bytes 32..47 reserved (HMAC-covered)
--   w12..w15 bytes 48..63 hmac tag (128 bits)
--
-- VHDL-2008. 250 MHz, single clock, synchronous active-high reset. No latches.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity param_block_loader is port( clk, rst : in std_logic;
  s_wr_en : in std_logic; s_wr_addr : in std_logic_vector(3 downto 0); s_wr_data : in std_logic_vector(31 downto 0);
  s_commit : in std_logic; now_ns : in ts_t; pblk_out : out param_block_t; load_err : out std_logic_vector(1 downto 0) ); end entity;

architecture rtl of param_block_loader is

  type word_array_t is array(natural range <>) of std_logic_vector(31 downto 0);

  -- Provisioned HMAC key (must equal the host signing key, host/ipc/load_hmac_key).
  -- Here: the 0xAB*32 dev key the host/tests use; in production source from eFuse.
  constant HMAC_KEY : std_logic_vector(255 downto 0) :=
    x"abababababababababababababababababababababababababababababababab";

  -- Byte-swap a 32-bit word: the CPU writes each word little-endian, but the host
  -- hashes the ParamBlock in MEMORY byte order (byte0 first). bswap(w) puts byte0
  -- (w[7:0]) in the MSB so hmac_data reproduces the host's byte[0..47] stream.
  function bswap(w : std_logic_vector(31 downto 0)) return std_logic_vector is
  begin
    return w(7 downto 0) & w(15 downto 8) & w(23 downto 16) & w(31 downto 24);
  end function;

  type ldr_state_t is (S_IDLE, S_START, S_WAIT, S_DONE);
  signal state    : ldr_state_t := S_IDLE;

  signal staging  : word_array_t(0 to 15) := (others => (others => '0'));
  signal snap     : word_array_t(0 to 15) := (others => (others => '0'));

  signal out_blk  : param_block_t := PARAM_BLOCK_INIT;
  signal err_reg  : std_logic_vector(1 downto 0) := "00";

  -- hmac_verify interface.
  signal hmac_start : std_logic := '0';
  signal hmac_data  : std_logic_vector(383 downto 0);
  signal hmac_tag   : std_logic_vector(127 downto 0);
  signal hmac_done  : std_logic;
  signal hmac_ok    : std_logic;

begin

  -- Registered outputs.
  pblk_out <= out_blk;
  load_err <= err_reg;

  -- Combinationally present the snapshot to the verifier: signed region
  -- (words 0..11) as 384-bit data, tag (words 12..15) as 128-bit tag.
  hmac_data <= bswap(snap(0)) & bswap(snap(1)) & bswap(snap(2))  & bswap(snap(3))  &
               bswap(snap(4)) & bswap(snap(5)) & bswap(snap(6))  & bswap(snap(7))  &
               bswap(snap(8)) & bswap(snap(9)) & bswap(snap(10)) & bswap(snap(11));
  hmac_tag  <= bswap(snap(12)) & bswap(snap(13)) & bswap(snap(14)) & bswap(snap(15));

  u_hmac : entity work.hmac_verify
    port map( clk => clk, rst => rst, start => hmac_start,
              data => hmac_data, key => HMAC_KEY, tag => hmac_tag,
              done => hmac_done, ok => hmac_ok );

  process(clk)
    variable epoch_v : unsigned(63 downto 0);
    variable age_v   : unsigned(63 downto 0);
  begin
    if rising_edge(clk) then
      if rst = '1' then
        state      <= S_IDLE;
        out_blk    <= PARAM_BLOCK_INIT;
        err_reg    <= "00";
        hmac_start <= '0';
        staging    <= (others => (others => '0'));
        snap       <= (others => (others => '0'));
      else
        hmac_start <= '0';             -- default: start is a 1-cycle pulse

        -- Word write port is always live (CPU may refill staging anytime).
        if s_wr_en = '1' then
          staging(to_integer(unsigned(s_wr_addr))) <= s_wr_data;
        end if;

        case state is

          when S_IDLE =>
            if s_commit = '1' then
              snap  <= staging;        -- latch a coherent snapshot
              state <= S_START;
            elsif out_blk.valid = '1' then
              -- Continuous staleness auto-kill on the active block (§7.3).
              epoch_v := out_blk.epoch_ns;
              if now_ns >= epoch_v then
                age_v := now_ns - epoch_v;
                if age_v > STALENESS_NS then
                  out_blk.kill_switch <= '1';
                  err_reg             <= "10";
                end if;
              end if;
            end if;

          when S_START =>
            hmac_start <= '1';
            state      <= S_WAIT;

          when S_WAIT =>
            if hmac_done = '1' then
              state <= S_DONE;
            end if;

          when S_DONE =>
            if hmac_ok = '0' then
              -- Authentication failed: reject and force safe state.
              err_reg             <= "01";
              out_blk.kill_switch <= '1';
            else
              epoch_v := unsigned(snap(7)) & unsigned(snap(6));
              if (now_ns >= epoch_v) and ((now_ns - epoch_v) > STALENESS_NS) then
                -- Authentic but stale: reject and force safe state.
                err_reg             <= "10";
                out_blk.kill_switch <= '1';
              else
                -- Authentic and fresh: atomically swap in the staged block.
                out_blk.inv_skew_q31  <= signed(snap(0));
                out_blk.half_spread   <= signed(snap(1));
                out_blk.max_inv       <= unsigned(snap(2));
                out_blk.base_qty      <= unsigned(snap(3));
                out_blk.regime_id     <= snap(4)(7 downto 0);
                -- kill_switch byte (byte 17) nonzero => kill.
                if snap(4)(15 downto 8) /= x"00" then
                  out_blk.kill_switch <= '1';
                else
                  out_blk.kill_switch <= '0';
                end if;
                out_blk.bid_skew      <= signed(snap(4)(31 downto 16));
                out_blk.ask_skew      <= signed(snap(5)(15 downto 0));
                out_blk.requote_ticks <= unsigned(snap(5)(31 downto 16));
                out_blk.epoch_ns      <= epoch_v;
                out_blk.valid         <= '1';
                err_reg               <= "00";
              end if;
            end if;
            state <= S_IDLE;

        end case;
      end if;
    end if;
  end process;

end architecture;
