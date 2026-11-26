-- MMX — fpga/rtl/quote_decider_buggy.vhd
-- L2 "tester's test" fixture: a DELIBERATELY BUGGY copy of quote_decider.
--
-- This file is NOT part of the shipping design. tb_quote_decider_l2 drives it and
-- asserts the L1-style op-code check CATCHES the bug (L2 PASS == bug detected).
--
-- ---- INJECTED BUG (exactly one) ---------------------------------------------
-- When a re-quote is required AND an order already rests, the spec says emit
-- OP_REPLACE (cancel-replace in one action). This buggy copy emits OP_PLACE
-- instead, i.e. it forgets that an order is already resting. Everything else is
-- a verbatim copy of quote_decider.vhd. See the "L2 INJECTED BUG" marker below.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity quote_decider_buggy is port( clk, rst : in std_logic;
  quote_in : in quote_t; quote_valid : in std_logic; now_ns : in ts_t; q : in tick_t; pblk : in param_block_t;
  action : out order_action_t; action_valid : out std_logic ); end entity;

architecture beh of quote_decider_buggy is

  signal bid_old       : tick_t := (others => '0');
  signal ask_old       : tick_t := (others => '0');
  signal last_quote_ns : ts_t   := (others => '0');
  signal resting       : std_logic := '0';
  signal cur_ref       : unsigned(31 downto 0) := (others => '0');
  signal next_ref      : unsigned(31 downto 0) := (others => '0');

  signal action_r      : order_action_t := (op => OP_NONE,
                                            bid => (others => '0'),
                                            ask => (others => '0'),
                                            qty => (others => '0'),
                                            ref => (others => '0'));
  signal action_valid_r: std_logic := '0';

  function absdiff33(a, b : tick_t) return signed is
    variable d : signed(TICK_W downto 0);
  begin
    d := resize(a, TICK_W+1) - resize(b, TICK_W+1);
    if d < 0 then
      return -d;
    else
      return d;
    end if;
  end function;

begin

  action       <= action_r;
  action_valid <= action_valid_r;

  process(clk)
    variable q_ext     : signed(TS_W downto 0);
    variable cap_ext   : signed(TS_W downto 0);
    variable rq_ext    : signed(TICK_W+1 downto 0);
    variable age        : ts_t;
    variable nr         : boolean;
  begin
    if rising_edge(clk) then
      if rst = '1' then
        bid_old        <= (others => '0');
        ask_old        <= (others => '0');
        last_quote_ns  <= (others => '0');
        resting        <= '0';
        cur_ref        <= (others => '0');
        next_ref       <= (others => '0');
        action_r.op    <= OP_NONE;
        action_r.bid   <= (others => '0');
        action_r.ask   <= (others => '0');
        action_r.qty   <= (others => '0');
        action_r.ref   <= (others => '0');
        action_valid_r <= '0';
      else
        action_valid_r <= '0';
        action_r.op    <= OP_NONE;
        action_r.bid   <= bid_old;
        action_r.ask   <= ask_old;
        action_r.qty   <= quote_in.qty;
        action_r.ref   <= std_logic_vector(cur_ref);

        if quote_valid = '1' then
          action_valid_r <= '1';

          if quote_in.cancel_only = '1' then
            action_r.op  <= OP_CANCEL;
            action_r.bid <= bid_old;
            action_r.ask <= ask_old;
            action_r.qty <= (others => '0');
            action_r.ref <= std_logic_vector(cur_ref);
            resting      <= '0';
          else
            q_ext   := resize(q, TS_W+1);
            cap_ext := signed(resize(pblk.max_inv, TS_W+1));
            rq_ext  := signed(resize(pblk.requote_ticks, TICK_W+2));

            nr := false;
            if pblk.kill_switch = '1' then
              nr := true;
            elsif q_ext > cap_ext then
              nr := true;
            elsif q_ext < -cap_ext then
              nr := true;
            else
              if now_ns >= last_quote_ns then
                age := now_ns - last_quote_ns;
                if age > MAX_QUOTE_AGE_NS then
                  nr := true;
                end if;
              end if;
              if resize(absdiff33(quote_in.bid, bid_old), TICK_W+2) > rq_ext then
                nr := true;
              end if;
              if resize(absdiff33(quote_in.ask, ask_old), TICK_W+2) > rq_ext then
                nr := true;
              end if;
            end if;

            if nr then
              -- L2 INJECTED BUG: should be OP_REPLACE when resting='1', but this
              -- copy always emits OP_PLACE (ignores the resting order).
              if resting = '1' then
                action_r.op <= OP_PLACE;
              else
                action_r.op <= OP_PLACE;
              end if;
              action_r.bid <= quote_in.bid;
              action_r.ask <= quote_in.ask;
              action_r.qty <= quote_in.qty;
              action_r.ref <= std_logic_vector(next_ref);
              bid_old       <= quote_in.bid;
              ask_old       <= quote_in.ask;
              last_quote_ns <= now_ns;
              resting       <= '1';
              cur_ref       <= next_ref;
              next_ref      <= next_ref + 1;
            else
              action_r.op  <= OP_NONE;
            end if;
          end if;
        end if;
      end if;
    end if;
  end process;

end architecture beh;
