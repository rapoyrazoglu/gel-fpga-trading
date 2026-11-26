-- MMX — fpga/rtl/ouch_encoder.vhd
-- Real OUCH-subset order encoder (spec §4.6, "order_encoder — wire format out").
--
-- Consumes one order_action_t from quote_decider and emits a Nasdaq-OUCH-4.2
-- "Enter Order" (type 'O') or "Cancel Order" (type 'X') message onto the TX
-- AXI-Stream (m_t*), honouring m_tready back-pressure and asserting m_tlast on
-- the final beat. Integer-only, registered AXIS outputs, no inferred latches,
-- single 250 MHz clock with synchronous active-high reset.
--
-- ---- WHY A 24-BYTE SUBSET ---------------------------------------------------
-- Full OUCH 4.2 Enter Order is 49 bytes with a 14-byte order token and an
-- 8-byte stock symbol. For the MMX data plane the symbol is fixed per session
-- and the token is a 32-bit rolling counter, so we ship a documented 24-byte
-- (= 3 x 64-bit beats) subset carrying exactly the fields the matching engine
-- needs to reconstruct the full frame downstream. Because 24 is a multiple of
-- 8, m_tkeep = 0xFF on every beat.
--
-- ---- EXACT BYTE LAYOUT (big-endian: message byte 0 == m_tdata[63:56]) --------
-- The message is packed MSB-first: byte 0 occupies the most-significant byte of
-- beat 0, byte 8 the MS byte of beat 1, byte 16 the MS byte of beat 2.
--
--   byte  0      : msg_type        'O'=0x4F Enter / 'X'=0x58 Cancel / 'U'=0x55 Replace
--   bytes 1..4   : order_token     32-bit, from the 'token' port (big-endian)
--   byte  5      : side            'B'=0x42 (buy) / 'S'=0x53 (sell) / 0x00 on cancel
--   bytes 6..9   : shares (qty)    32-bit unsigned, from action.qty (big-endian)
--   bytes 10..13 : price           32-bit signed ticks, from action.bid (big-endian)
--   byte  14     : time_in_force   0x00 = Day
--   byte  15     : capacity/flags  0x50 = 'P' post-only
--   bytes 16..23 : reserved        0x00 padding (symbol/firm carried out-of-band)
--
-- Mapping order_action_t -> message:
--   OP_PLACE   : type 'O', side 'B', price = action.bid, shares = action.qty.
--                (This module emits the BID / BUY leg of the two-sided quote;
--                 the ASK leg is a second message in a fuller build — see risks.)
--   OP_CANCEL  : type 'X', side 0x00, price = 0, shares = action.qty (cancel qty),
--                token identifies the resting order to pull.
--   OP_REPLACE : type 'U', otherwise identical field mapping to OP_PLACE.
--   OP_NONE / anything else : no frame emitted.
--
-- A one-deep pending buffer latches an action that arrives while a frame is
-- still streaming (keeps newest), so no upstream ready is needed.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity ouch_encoder is port( clk, rst : in std_logic;
  action : in order_action_t; action_valid : in std_logic;
  token : in std_logic_vector(31 downto 0);
  m_tdata : out std_logic_vector(63 downto 0); m_tkeep : out std_logic_vector(7 downto 0);
  m_tvalid : out std_logic; m_tlast : out std_logic; m_tready : in std_logic ); end entity;

architecture beh of ouch_encoder is

  constant N_BEATS  : natural := 3;                 -- 24 bytes / 8
  constant MSG_W    : natural := N_BEATS*64;        -- 192 bits
  constant LAST_IDX : unsigned(1 downto 0) := to_unsigned(N_BEATS-1, 2);

  type beat_array_t is array(0 to N_BEATS-1) of std_logic_vector(63 downto 0);

  -- Build the 24-byte OUCH-subset message as one big-endian bit vector, then
  -- slice it into 64-bit beats. Pure combinational helper (single-cycle stamp).
  function build_msg(op    : order_op_t;
                     bidp  : tick_t;
                     qtyv  : qty_t;
                     tok   : std_logic_vector(31 downto 0))
                     return std_logic_vector is
    variable m     : std_logic_vector(MSG_W-1 downto 0) := (others => '0');
    variable mtype : std_logic_vector(7 downto 0);
    variable side  : std_logic_vector(7 downto 0);
    variable price : std_logic_vector(31 downto 0);
    variable tif   : std_logic_vector(7 downto 0) := x"00";  -- Day
    variable cap   : std_logic_vector(7 downto 0) := x"50";  -- 'P' post-only
  begin
    case op is
      when OP_PLACE   => mtype := x"4F"; side := x"42"; price := std_logic_vector(bidp); -- 'O','B'
      when OP_REPLACE => mtype := x"55"; side := x"42"; price := std_logic_vector(bidp); -- 'U','B'
      when OP_CANCEL  => mtype := x"58"; side := x"00"; price := (others => '0');        -- 'X'
      when others     => mtype := x"00"; side := x"00"; price := (others => '0');
    end case;
    -- byte 0
    m(191 downto 184) := mtype;
    -- bytes 1..4 : order token
    m(183 downto 152) := tok;
    -- byte 5 : side
    m(151 downto 144) := side;
    -- bytes 6..9 : shares
    m(143 downto 112) := std_logic_vector(qtyv);
    -- bytes 10..13 : price
    m(111 downto 80)  := price;
    -- byte 14 : TIF
    m(79 downto 72)   := tif;
    -- byte 15 : capacity/flags
    m(71 downto 64)   := cap;
    -- bytes 16..23 : reserved (already zero)
    return m;
  end function;

  function slice_beats(m : std_logic_vector(MSG_W-1 downto 0)) return beat_array_t is
    variable b : beat_array_t;
  begin
    for i in 0 to N_BEATS-1 loop
      -- beat 0 = MS 64 bits (bytes 0..7); big-endian, byte 0 first on the wire.
      b(i) := m(MSG_W-1 - i*64 downto MSG_W-64 - i*64);
    end loop;
    return b;
  end function;

  type state_t is (S_IDLE, S_STREAM);
  signal state    : state_t := S_IDLE;
  signal buf      : beat_array_t := (others => (others => '0'));
  signal beat_idx : unsigned(1 downto 0) := (others => '0');

  -- One-deep pending action (captures a newer action arriving mid-stream).
  signal pend_valid : std_logic := '0';
  signal pend_act   : order_action_t := (op => OP_NONE, bid => (others => '0'),
                                         ask => (others => '0'), qty => (others => '0'),
                                         ref => (others => '0'));
  signal pend_tok   : std_logic_vector(31 downto 0) := (others => '0');

  -- Registered AXIS outputs.
  signal m_tdata_r  : std_logic_vector(63 downto 0) := (others => '0');
  signal m_tvalid_r : std_logic := '0';
  signal m_tlast_r  : std_logic := '0';

begin

  m_tdata  <= m_tdata_r;
  m_tkeep  <= x"FF";          -- all 8 lanes valid on every beat (24 = 3*8)
  m_tvalid <= m_tvalid_r;
  m_tlast  <= m_tlast_r;

  process(clk)
    variable nb   : beat_array_t;
    variable nxt  : unsigned(1 downto 0);
    variable go   : boolean;
    variable g_op : order_op_t;
    variable g_bid: tick_t;
    variable g_qty: qty_t;
    variable g_tok: std_logic_vector(31 downto 0);
  begin
    if rising_edge(clk) then
      if rst = '1' then
        state      <= S_IDLE;
        buf        <= (others => (others => '0'));
        beat_idx   <= (others => '0');
        pend_valid <= '0';
        pend_act   <= (op => OP_NONE, bid => (others => '0'), ask => (others => '0'),
                       qty => (others => '0'), ref => (others => '0'));
        pend_tok   <= (others => '0');
        m_tdata_r  <= (others => '0');
        m_tvalid_r <= '0';
        m_tlast_r  <= '0';
      else
        case state is

          ----------------------------------------------------------------------
          when S_IDLE =>
            m_tvalid_r <= '0';
            m_tlast_r  <= '0';
            go := false;
            if pend_valid = '1' then
              go    := true;
              g_op  := pend_act.op;  g_bid := pend_act.bid;
              g_qty := pend_act.qty; g_tok := pend_tok;
              pend_valid <= '0';
            elsif action_valid = '1' and action.op /= OP_NONE then
              go    := true;
              g_op  := action.op;  g_bid := action.bid;
              g_qty := action.qty; g_tok := token;
            end if;

            if go then
              nb         := slice_beats(build_msg(g_op, g_bid, g_qty, g_tok));
              buf        <= nb;
              beat_idx   <= (others => '0');
              m_tdata_r  <= nb(0);
              m_tvalid_r <= '1';
              if N_BEATS = 1 then
                m_tlast_r <= '1';
              else
                m_tlast_r <= '0';
              end if;
              state      <= S_STREAM;
            end if;

          ----------------------------------------------------------------------
          when S_STREAM =>
            -- Capture a newer action while busy (keep newest, one-deep).
            if action_valid = '1' and action.op /= OP_NONE then
              pend_valid <= '1';
              pend_act   <= action;
              pend_tok   <= token;
            end if;

            -- Advance only on a completed beat handshake.
            if m_tvalid_r = '1' and m_tready = '1' then
              if beat_idx = LAST_IDX then
                if pend_valid = '1' then
                  nb         := slice_beats(build_msg(pend_act.op, pend_act.bid,
                                                      pend_act.qty, pend_tok));
                  buf        <= nb;
                  beat_idx   <= (others => '0');
                  m_tdata_r  <= nb(0);
                  m_tvalid_r <= '1';
                  m_tlast_r  <= '0';
                  pend_valid <= '0';
                  state      <= S_STREAM;
                else
                  m_tvalid_r <= '0';
                  m_tlast_r  <= '0';
                  state      <= S_IDLE;
                end if;
              else
                nxt        := beat_idx + 1;
                beat_idx   <= nxt;
                m_tdata_r  <= buf(to_integer(nxt));
                m_tvalid_r <= '1';
                if nxt = LAST_IDX then
                  m_tlast_r <= '1';
                else
                  m_tlast_r <= '0';
                end if;
              end if;
            end if;

        end case;
      end if;
    end if;
  end process;

end architecture beh;
