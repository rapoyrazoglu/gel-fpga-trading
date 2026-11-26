-- MMX — fpga/rtl/order_encoder_buggy.vhd
-- L2 "tester's test" fixture: a DELIBERATELY BUGGY copy of order_encoder.
--
-- This file is NOT part of the shipping design. It exists so tb_order_encoder_l2
-- can prove the L1-style field checks are load-bearing: it feeds a known bug
-- into the encoder and asserts the checks CATCH it (L2 PASS == bug detected).
--
-- ---- INJECTED BUG (exactly one) ---------------------------------------------
-- build_frame() packs beat 2 with the ASK price in the high word and the BID
-- price in the low word — the two legs are SWAPPED relative to the spec layout
-- (beat 2 : [63:32] bid price / [31:0] ask price). Everything else is a verbatim
-- copy of order_encoder.vhd. See the "L2 INJECTED BUG" marker below.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity order_encoder_buggy is port( clk, rst : in std_logic;
  action : in order_action_t; action_valid : in std_logic; session_id : in std_logic_vector(31 downto 0);
  m_tdata : out std_logic_vector(63 downto 0); m_tkeep : out std_logic_vector(7 downto 0);
  m_tvalid : out std_logic; m_tlast : out std_logic; m_tready : in std_logic ); end entity;

architecture beh of order_encoder_buggy is

  constant N_BEATS  : natural := 8;
  constant LAST_IDX : unsigned(2 downto 0) := to_unsigned(N_BEATS-1, 3);

  type beat_array_t is array(0 to N_BEATS-1) of std_logic_vector(63 downto 0);

  constant TEMPLATE : beat_array_t := (
    0 => (others => '0'),
    1 => (others => '0'),
    2 => (others => '0'),
    3 => (others => '0'),
    4 => x"4D_4D_58_30_00_00_00_00",   -- "MMX0" firm id + reserved
    5 => x"00_00_00_00_00_00_00_00",   -- symbol id (set by deployment/BRAM)
    6 => x"44_41_59_01_50_00_00_00",   -- 'D''A''Y', POST_ONLY=1, 'P', pad
    7 => x"00_00_00_00_00_00_00_00");  -- trailer / padding

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
    -- L2 INJECTED BUG: bid/ask legs swapped (spec is bid in [63:32], ask in [31:0]).
    f(2) := std_logic_vector(ask) & std_logic_vector(bid);
    f(3) := std_logic_vector(qty) & x"00000000";
    return f;
  end function;

  type state_t is (S_IDLE, S_STREAM);
  signal state    : state_t := S_IDLE;
  signal buf      : beat_array_t := TEMPLATE;
  signal beat_idx : unsigned(2 downto 0) := (others => '0');

  signal pend_valid : std_logic := '0';
  signal pend_act   : order_action_t := (op => OP_NONE,
                                         bid => (others => '0'),
                                         ask => (others => '0'),
                                         qty => (others => '0'),
                                         ref => (others => '0'));
  signal pend_sess  : std_logic_vector(31 downto 0) := (others => '0');

  signal m_tdata_r  : std_logic_vector(63 downto 0) := (others => '0');
  signal m_tvalid_r : std_logic := '0';
  signal m_tlast_r  : std_logic := '0';

begin

  m_tdata  <= m_tdata_r;
  m_tkeep  <= x"FF";
  m_tvalid <= m_tvalid_r;
  m_tlast  <= m_tlast_r;

  process(clk)
    variable nb        : beat_array_t;
    variable nxt       : unsigned(2 downto 0);
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

          when S_IDLE =>
            m_tvalid_r <= '0';
            m_tlast_r  <= '0';
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

          when S_STREAM =>
            if action_valid = '1' and action.op /= OP_NONE then
              pend_valid <= '1';
              pend_act   <= action;
              pend_sess  <= session_id;
            end if;

            if m_tvalid_r = '1' and m_tready = '1' then
              if beat_idx = LAST_IDX then
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
