-- MMX — fpga/rtl/quote_decider.vhd
-- Cancel/replace gate (spec §4.5, policy §3.5).
--
-- VHDL-2008 mirror of mmx::needs_replace() in host/pricing/pricing_engine.h.
-- Holds the currently-resting quote (bid_old, ask_old, last_quote_ns) plus a
-- "resting" flag in registers. On each fresh quote it decides one of:
--
--   need_replace = kill_switch
--               OR (|q|  > max_inv)
--               OR (now_ns - last_quote_ns > MAX_QUOTE_AGE_NS)   [guarded vs wrap]
--               OR (|bid - bid_old| > requote_ticks)
--               OR (|ask - ask_old| > requote_ticks)
--
--   quote_in.cancel_only        -> OP_CANCEL  (pull resting quote, no new order)
--   need_replace & none resting -> OP_PLACE   (first / re-arm)
--   need_replace & resting      -> OP_REPLACE (cancel-replace in one action)
--   otherwise                   -> OP_NONE
--
-- All comparisons promoted to wide signed/unsigned to match the host's int64
-- arithmetic (max_inv can exceed INT32_MAX; abs-diffs can exceed INT32 range).
-- Single 250 MHz clock, sync active-high reset, registered outputs, no latches.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity quote_decider is port( clk, rst : in std_logic;
  quote_in : in quote_t; quote_valid : in std_logic; now_ns : in ts_t; q : in tick_t; pblk : in param_block_t;
  action : out order_action_t; action_valid : out std_logic ); end entity;

architecture beh of quote_decider is

  -- Resting-quote state (§4.5).
  signal bid_old       : tick_t := (others => '0');
  signal ask_old       : tick_t := (others => '0');
  signal last_quote_ns : ts_t   := (others => '0');
  signal resting       : std_logic := '0';        -- an order is live in the book
  signal cur_ref       : unsigned(31 downto 0) := (others => '0'); -- resting order id
  signal next_ref      : unsigned(31 downto 0) := (others => '0'); -- next id to assign

  -- Registered outputs.
  signal action_r      : order_action_t := (op => OP_NONE,
                                            bid => (others => '0'),
                                            ask => (others => '0'),
                                            qty => (others => '0'),
                                            ref => (others => '0'));
  signal action_valid_r: std_logic := '0';

  -- |a-b| in 33-bit signed (one guard bit so the subtract cannot overflow).
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
    -- wide-domain temporaries mirroring host int64 compares
    variable q_ext     : signed(TS_W downto 0);          -- 65-bit signed inventory
    variable cap_ext   : signed(TS_W downto 0);          -- 65-bit signed max_inv
    variable rq_ext    : signed(TICK_W+1 downto 0);      -- requote_ticks as signed
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
        -- default: no action this cycle (every signal assigned on every path)
        action_valid_r <= '0';
        action_r.op    <= OP_NONE;
        action_r.bid   <= bid_old;
        action_r.ask   <= ask_old;
        action_r.qty   <= quote_in.qty;
        action_r.ref   <= std_logic_vector(cur_ref);

        if quote_valid = '1' then
          action_valid_r <= '1';

          if quote_in.cancel_only = '1' then
            -- Kill-all path: cancel anything resting, place nothing.
            action_r.op  <= OP_CANCEL;
            action_r.bid <= bid_old;
            action_r.ask <= ask_old;
            action_r.qty <= (others => '0');
            action_r.ref <= std_logic_vector(cur_ref);
            resting      <= '0';
          else
            -- ---- need_replace, mirroring needs_replace() exactly ----------
            q_ext   := resize(q, TS_W+1);
            cap_ext := signed(resize(pblk.max_inv, TS_W+1));   -- top bit 0 => +ve
            rq_ext  := signed(resize(pblk.requote_ticks, TICK_W+2));

            nr := false;
            if pblk.kill_switch = '1' then
              nr := true;
            elsif q_ext > cap_ext then
              nr := true;
            elsif q_ext < -cap_ext then
              nr := true;
            else
              -- age check, guarded against unsigned wrap (only if now >= last)
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
              -- Re-quote: REPLACE if an order rests, else first PLACE.
              if resting = '1' then
                action_r.op <= OP_REPLACE;
              else
                action_r.op <= OP_PLACE;
              end if;
              action_r.bid <= quote_in.bid;
              action_r.ask <= quote_in.ask;
              action_r.qty <= quote_in.qty;
              action_r.ref <= std_logic_vector(next_ref);
              -- commit new resting quote
              bid_old       <= quote_in.bid;
              ask_old       <= quote_in.ask;
              last_quote_ns <= now_ns;
              resting       <= '1';
              cur_ref       <= next_ref;
              next_ref      <= next_ref + 1;
            else
              action_r.op  <= OP_NONE;   -- inside band: leave resting quote alone
            end if;
          end if;
        end if;
      end if;
    end if;
  end process;

end architecture beh;
