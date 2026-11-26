-- MMX — fpga/rtl/order_encoder.vhd
-- Wire-format-out stage (spec §4.6 "order_encoder — wire format out").
--
-- Consumes one order_action_t from quote_decider and emits an OUCH-like order
-- frame on the TX AXI-Stream (m_t*). Per spec §4.6:
--   * a pre-built header TEMPLATE lives in registers (cheap BRAM-equivalent for
--     a 64-byte frame); only the price / qty / side / ref / op / session fields
--     are mutated, so the frame is built in a SINGLE cycle, then streamed by a
--     small FSM that asserts m_tlast on the final beat and honours m_tready
--     back-pressure.
--   * supports OP_PLACE (enter), OP_CANCEL, and OP_REPLACE (cancel+place in one
--     frame). OP_NONE produces no frame.
--   * session_id is carried in the header.
--
-- ---- FRAME LAYOUT (64 bytes = 8 beats of 64 bits, big-endian-in-beat) --------
-- The frame is an MMX OUCH-like "quote" message. order_action_t carries BOTH
-- legs (bid+ask) of a two-sided quote plus the resting ref id, so one frame
-- expresses the whole requote. Byte 0 is the OUCH message-type letter.
--
--   beat 0 : [63:56] msg_type  ('O'=0x4F enter / 'X'=0x58 cancel / 'U'=0x55 replace)
--            [55:48] op_code   (1=PLACE, 2=CANCEL, 3=REPLACE)
--            [47:32] reserved  (0x0000)
--            [31: 0] session_id
--   beat 1 : [63:32] coid_hi   (0 — session counter high word, reserved)
--            [31: 0] ref       (client/ref order id from quote_decider)
--   beat 2 : [63:32] bid price (tick_t, signed)         <- BID leg / side=BUY
--            [31: 0] ask price (tick_t, signed)         <- ASK leg / side=SELL
--   beat 3 : [63:32] qty       (qty_t, unsigned, per-leg display/order qty)
--            [31: 0] reserved  (0x00000000)
--   beat 4 : account / firm id        (template, fixed boilerplate)
--   beat 5 : symbol / instrument id   (template)
--   beat 6 : TIF / capacity / flags   (template: 'DAY', POST_ONLY)
--   beat 7 : OUCH trailer / padding   (template)
--
-- Only beats 0..3 are mutated per action; beats 4..7 come straight from the
-- header TEMPLATE. m_tkeep is 0xFF on every beat (frame is a multiple of 8 B).
--
-- 250 MHz single clock, sync active-high reset, registered AXIS outputs, no
-- latches (every register assigned on every path). One-deep pending buffer
-- absorbs an action that arrives mid-stream (keeps newest), so no upstream
-- ready is required.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity order_encoder is port( clk, rst : in std_logic;
  action : in order_action_t; action_valid : in std_logic; session_id : in std_logic_vector(31 downto 0);
  m_tdata : out std_logic_vector(63 downto 0); m_tkeep : out std_logic_vector(7 downto 0);
  m_tvalid : out std_logic; m_tlast : out std_logic; m_tready : in std_logic ); end entity;

architecture beh of order_encoder is

  constant N_BEATS  : natural := 8;
  constant LAST_IDX : unsigned(2 downto 0) := to_unsigned(N_BEATS-1, 3);

  type beat_array_t is array(0 to N_BEATS-1) of std_logic_vector(63 downto 0);

  -- Pre-built header template (spec §4.6). Beats 0..3 are overwritten per
  -- action; beats 4..7 are constant boilerplate shipped verbatim every frame.
  --   beat 4: account/firm "MMX0"            beat 5: symbol id placeholder
  --   beat 6: TIF='DAY' (0x444159), POST_ONLY=0x01, capacity='P'(0x50)
  --   beat 7: trailer / zero padding
  constant TEMPLATE : beat_array_t := (
    0 => (others => '0'),
    1 => (others => '0'),
    2 => (others => '0'),
    3 => (others => '0'),
    4 => x"4D_4D_58_30_00_00_00_00",   -- "MMX0" firm id + reserved
    5 => x"00_00_00_00_00_00_00_00",   -- symbol id (set by deployment/BRAM)
    6 => x"44_41_59_01_50_00_00_00",   -- 'D''A''Y', POST_ONLY=1, 'P', pad
    7 => x"00_00_00_00_00_00_00_00");  -- trailer / padding

  -- Single-cycle field mutation: stamp action fields onto the template.
  function build_frame(op      : order_op_t;
                       bid, ask : tick_t;
                       qty      : qty_t;
                       ref      : std_logic_vector(31 downto 0);
                       session  : std_logic_vector(31 downto 0)) return beat_array_t is
    variable f  : beat_array_t := TEMPLATE;
    variable mt : std_logic_vector(7 downto 0);
    variable oc : std_logic_vector(7 downto 0);
  begin
    case op is
      when OP_PLACE   => mt := x"4F"; oc := x"01";  -- 'O' enter
      when OP_CANCEL  => mt := x"58"; oc := x"02";  -- 'X' cancel
      when OP_REPLACE => mt := x"55"; oc := x"03";  -- 'U' replace
      when others     => mt := x"00"; oc := x"00";
    end case;
    f(0) := mt & oc & x"0000" & session;
    f(1) := x"00000000" & ref;
    f(2) := std_logic_vector(bid) & std_logic_vector(ask);
    f(3) := std_logic_vector(qty) & x"00000000";
    return f;
  end function;

  type state_t is (S_IDLE, S_STREAM);
  signal state    : state_t := S_IDLE;
  signal buf      : beat_array_t := TEMPLATE;     -- frame being streamed
  signal beat_idx : unsigned(2 downto 0) := (others => '0');

  -- One-deep pending action (captures a newer action arriving mid-stream).
  signal pend_valid : std_logic := '0';
  signal pend_act   : order_action_t := (op => OP_NONE,
                                         bid => (others => '0'),
                                         ask => (others => '0'),
                                         qty => (others => '0'),
                                         ref => (others => '0'));
  signal pend_sess  : std_logic_vector(31 downto 0) := (others => '0');

  -- Registered AXIS outputs.
  signal m_tdata_r  : std_logic_vector(63 downto 0) := (others => '0');
  signal m_tvalid_r : std_logic := '0';
  signal m_tlast_r  : std_logic := '0';

begin

  m_tdata  <= m_tdata_r;
  m_tkeep  <= x"FF";          -- all 8 lanes valid on every beat
  m_tvalid <= m_tvalid_r;
  m_tlast  <= m_tlast_r;

  process(clk)
    variable nb        : beat_array_t;
    variable nxt       : unsigned(2 downto 0);
    -- which action to launch next (current input or buffered pending)
    variable go        : boolean;
    variable g_op      : order_op_t;
    variable g_bid     : tick_t;
    variable g_ask     : tick_t;
    variable g_qty     : qty_t;
    variable g_ref     : std_logic_vector(31 downto 0);
    variable g_sess    : std_logic_vector(31 downto 0);
  begin
    if rising_edge(clk) then
      if rst = '1' then
        state      <= S_IDLE;
        buf        <= TEMPLATE;
        beat_idx   <= (others => '0');
        pend_valid <= '0';
        pend_act   <= (op => OP_NONE, bid => (others => '0'), ask => (others => '0'),
                       qty => (others => '0'), ref => (others => '0'));
        pend_sess  <= (others => '0');
        m_tdata_r  <= (others => '0');
        m_tvalid_r <= '0';
        m_tlast_r  <= '0';
      else
        case state is

          ----------------------------------------------------------------------
          when S_IDLE =>
            m_tvalid_r <= '0';
            m_tlast_r  <= '0';
            -- Launch on a real action (op /= OP_NONE). A pending action latched
            -- earlier takes priority, then a fresh one on the port.
            go := false;
            if pend_valid = '1' then
              go     := true;
              g_op   := pend_act.op;  g_bid := pend_act.bid; g_ask := pend_act.ask;
              g_qty  := pend_act.qty; g_ref := pend_act.ref; g_sess := pend_sess;
              pend_valid <= '0';
            elsif action_valid = '1' and action.op /= OP_NONE then
              go     := true;
              g_op   := action.op;  g_bid := action.bid; g_ask := action.ask;
              g_qty  := action.qty; g_ref := action.ref; g_sess := session_id;
            end if;

            if go then
              nb         := build_frame(g_op, g_bid, g_ask, g_qty, g_ref, g_sess);
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
              pend_sess  <= session_id;
            end if;

            -- Advance only on a completed beat handshake.
            if m_tvalid_r = '1' and m_tready = '1' then
              if beat_idx = LAST_IDX then
                -- Frame done. Chain straight into a pending action if present.
                if pend_valid = '1' then
                  nb         := build_frame(pend_act.op, pend_act.bid, pend_act.ask,
                                            pend_act.qty, pend_act.ref, pend_sess);
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
