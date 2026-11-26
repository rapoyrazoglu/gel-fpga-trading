-- MMX — fpga/rtl/feed_parser.vhd
-- Spec §4.2  feed_parser : wire format -> normalized feed_msg_t.
--
-- Implements the "ITCH-lite" branch of §4.2 (ITCH chosen — cheapest to build).
-- The incoming AXI-Stream (s_t*) carries a byte-packed payload produced by
-- mac_udp_rx (UDP payload only).  We re-serialize the 64-bit beats into a byte
-- stream and assemble a COMPACT FIXED-LAYOUT 20-byte message.  Big-endian
-- (network order: lowest byte offset = most-significant byte of a field).
-- AXI-Stream byte lane convention: lane 0 = s_tdata(7 downto 0) is the first
-- byte on the wire, lane 7 = s_tdata(63 downto 48) is the last.
--
--   off  size  field        notes
--   ---  ----  -----------  --------------------------------------------------
--    0    1    msg_type     MSG_ADD/MSG_DEL/MSG_EXEC/MSG_TRADE (mmx_pkg)
--    1    8    ts_ns        unsigned ns timestamp, big-endian
--    9    2    ticker_id    raw 16-bit symbol id
--   11    1    side         0x00 = BID, 0x01 = ASK
--   12    4    price        signed price in ticks, big-endian (two's-comp)
--   16    4    size         unsigned quantity, big-endian
--   ---  ----  -----------
--   20   total bytes
--
-- A 64-bit beat brings up to 8 valid bytes (per s_tkeep).  Since 8 < 20, a
-- single beat can complete AT MOST ONE message, so one msg_valid pulse per
-- beat is sufficient.  s_tlast resyncs the byte index to a packet boundary so
-- a dropped/short packet cannot desynchronize subsequent messages.
--
-- Fully registered, single clock (250 MHz), sync active-high reset, no latches,
-- numeric_std only — synthesizable RTL.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity feed_parser is port( clk, rst : in std_logic;
  s_tdata : in std_logic_vector(63 downto 0); s_tkeep : in std_logic_vector(7 downto 0);
  s_tvalid : in std_logic; s_tlast : in std_logic; s_tready : out std_logic;
  msg_out : out feed_msg_t; msg_valid : out std_logic ); end entity;

architecture rtl of feed_parser is

  constant MSG_BYTES : integer := 20;

  type byte_arr_t is array(0 to MSG_BYTES-1) of std_logic_vector(7 downto 0);

  signal buf  : byte_arr_t;
  signal bidx : integer range 0 to MSG_BYTES;  -- next byte position to fill

  signal msg_r   : feed_msg_t;
  signal mv_r    : std_logic;
  signal ready_r : std_logic;

  -- assemble a normalized feed_msg_t from the 20-byte buffer (big-endian).
  function assemble(b : byte_arr_t) return feed_msg_t is
    variable m : feed_msg_t;
  begin
    -- Convert each byte to unsigned before '&' so the concatenation resolves to
    -- numeric_std's operator (not the implicit byte_arr_t array-concat, which
    -- makes element&element ambiguous). Big-endian: byte 1 is the MSB.
    m.msg_type  := b(0);
    m.ts_ns     := unsigned(b(1)) & unsigned(b(2)) & unsigned(b(3)) & unsigned(b(4))
                 & unsigned(b(5)) & unsigned(b(6)) & unsigned(b(7)) & unsigned(b(8));
    m.ticker_id := std_logic_vector(unsigned(b(9)) & unsigned(b(10)));
    if b(11) = x"00" then m.side := BID; else m.side := ASK; end if;
    m.price     := signed(unsigned(b(12)) & unsigned(b(13)) & unsigned(b(14)) & unsigned(b(15)));
    m.size      := unsigned(b(16)) & unsigned(b(17)) & unsigned(b(18)) & unsigned(b(19));
    return m;
  end function;

begin

  -- We can always accept (sink-limited downstream consumers register msg_out).
  s_tready  <= ready_r;
  msg_out   <= msg_r;
  msg_valid <= mv_r;

  process(clk)
    variable v_buf      : byte_arr_t;
    variable v_idx      : integer range 0 to MSG_BYTES;
    variable v_byte     : std_logic_vector(7 downto 0);
    variable v_complete : boolean;
    variable v_msg      : feed_msg_t;
  begin
    if rising_edge(clk) then
      if rst = '1' then
        bidx    <= 0;
        mv_r    <= '0';
        ready_r <= '1';
        msg_r   <= feed_msg_t'(
                     ts_ns     => (others => '0'),
                     ticker_id => (others => '0'),
                     side      => BID,
                     price     => (others => '0'),
                     size      => (others => '0'),
                     msg_type  => (others => '0'));
        buf     <= (others => (others => '0'));
      else
        ready_r    <= '1';
        mv_r       <= '0';                 -- default: single-cycle pulse
        v_buf      := buf;
        v_idx      := bidx;
        v_complete := false;
        v_msg      := msg_r;

        if (s_tvalid = '1') and (ready_r = '1') then
          -- consume up to 8 byte lanes in wire order (lane 0 first).
          for lane in 0 to 7 loop
            if s_tkeep(lane) = '1' then
              v_byte := s_tdata(lane*8+7 downto lane*8);
              v_buf(v_idx) := v_byte;
              if v_idx = MSG_BYTES-1 then
                v_complete := true;
                v_msg      := assemble(v_buf);
                v_idx      := 0;
              else
                v_idx := v_idx + 1;
              end if;
            end if;
          end loop;

          -- resync to packet boundary on end-of-frame.
          if s_tlast = '1' then
            v_idx := 0;
          end if;
        end if;

        buf   <= v_buf;
        bidx  <= v_idx;
        msg_r <= v_msg;
        if v_complete then
          mv_r <= '1';
        end if;
      end if;
    end if;
  end process;

end architecture;
