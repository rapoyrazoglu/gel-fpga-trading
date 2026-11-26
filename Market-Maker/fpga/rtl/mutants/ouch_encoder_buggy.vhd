-- MMX — fpga/rtl/ouch_encoder_buggy.vhd
-- L2 "tester's test" fixture: a DELIBERATELY BUGGY copy of ouch_encoder.
--
-- This file is NOT part of the shipping design. tb_ouch_encoder_l2 drives it and
-- asserts the L1-style side-byte check CATCHES the bug (L2 PASS == bug detected).
--
-- ---- INJECTED BUG (exactly one) ---------------------------------------------
-- build_msg() stamps the OP_PLACE 'Enter Order' side byte as 'S' (0x53, SELL)
-- instead of 'B' (0x42, BUY) — the BUY leg's side is wrong. Everything else is a
-- verbatim copy of ouch_encoder.vhd. See the "L2 INJECTED BUG" marker below.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity ouch_encoder_buggy is port( clk, rst : in std_logic;
  action : in order_action_t; action_valid : in std_logic;
  token : in std_logic_vector(31 downto 0);
  m_tdata : out std_logic_vector(63 downto 0); m_tkeep : out std_logic_vector(7 downto 0);
  m_tvalid : out std_logic; m_tlast : out std_logic; m_tready : in std_logic ); end entity;

architecture beh of ouch_encoder_buggy is

  constant N_BEATS  : natural := 3;
  constant MSG_W    : natural := N_BEATS*64;
  constant LAST_IDX : unsigned(1 downto 0) := to_unsigned(N_BEATS-1, 2);

  type beat_array_t is array(0 to N_BEATS-1) of std_logic_vector(63 downto 0);

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
      -- L2 INJECTED BUG: OP_PLACE side should be 'B' (0x42) but is stamped 'S' (0x53).
      when OP_PLACE   => mtype := x"4F"; side := x"53"; price := std_logic_vector(bidp); -- 'O', WRONG 'S'
      when OP_REPLACE => mtype := x"55"; side := x"42"; price := std_logic_vector(bidp); -- 'U','B'
      when OP_CANCEL  => mtype := x"58"; side := x"00"; price := (others => '0');        -- 'X'
      when others     => mtype := x"00"; side := x"00"; price := (others => '0');
    end case;
    m(191 downto 184) := mtype;
    m(183 downto 152) := tok;
    m(151 downto 144) := side;
    m(143 downto 112) := std_logic_vector(qtyv);
    m(111 downto 80)  := price;
    m(79 downto 72)   := tif;
    m(71 downto 64)   := cap;
    return m;
  end function;

  function slice_beats(m : std_logic_vector(MSG_W-1 downto 0)) return beat_array_t is
    variable b : beat_array_t;
  begin
    for i in 0 to N_BEATS-1 loop
      b(i) := m(MSG_W-1 - i*64 downto MSG_W-64 - i*64);
    end loop;
    return b;
  end function;

  type state_t is (S_IDLE, S_STREAM);
  signal state    : state_t := S_IDLE;
  signal buf      : beat_array_t := (others => (others => '0'));
  signal beat_idx : unsigned(1 downto 0) := (others => '0');

  signal pend_valid : std_logic := '0';
  signal pend_act   : order_action_t := (op => OP_NONE, bid => (others => '0'),
                                         ask => (others => '0'), qty => (others => '0'),
                                         ref => (others => '0'));
  signal pend_tok   : std_logic_vector(31 downto 0) := (others => '0');

  signal m_tdata_r  : std_logic_vector(63 downto 0) := (others => '0');
  signal m_tvalid_r : std_logic := '0';
  signal m_tlast_r  : std_logic := '0';

begin

  m_tdata  <= m_tdata_r;
  m_tkeep  <= x"FF";
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

          when S_STREAM =>
            if action_valid = '1' and action.op /= OP_NONE then
              pend_valid <= '1';
              pend_act   <= action;
              pend_tok   <= token;
            end if;

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
