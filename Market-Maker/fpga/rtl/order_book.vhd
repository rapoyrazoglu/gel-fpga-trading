-- MMX — fpga/rtl/order_book.vhd
-- Top-N (N_LEVELS=8) dual-sided limit order book (spec §4.3).
--
--   feed_parser -> [order_book] --mid s--> pricing_engine_beh
--
-- Storage: 2 x N_LEVELS x {price, qty} held in registers (distributed RAM) for
-- single-cycle access. Bids are kept sorted descending (index 0 = best/highest
-- price), asks ascending (index 0 = best/lowest price) via a parallel shift
-- insert/remove. Each accepted feed message mutates exactly one side:
--   ADD   (0x41 'A') : insert a new price level, or accumulate qty if present.
--   DEL   (0x44 'D') : remove the level at that price (shift up).
--   EXEC  (0x45 'E') : reduce qty at that price; drop the level when it hits 0.
--   TRADE (0x50 'P') : same reduce-at-price semantics as EXEC.
-- After every accepted message we emit book_out_t and pulse book_valid:
--   mid       = mid_of(best_bid, best_ask)          (package helper, arith /2)
--   spread    = best_ask - best_bid
--   imbalance = coarse Q1.15 of (bidQ-askQ)/(bidQ+askQ), division-free:
--               normalise by the nearest power-of-two of the qty sum, clamp.
-- Empty side is handled gracefully: the last valid mid is held, spread forced 0.
--
-- VHDL-2008, single clock 'clk' @250 MHz, synchronous active-high 'rst'.
-- Fully registered outputs, no latches, numeric_std only.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity order_book is port( clk, rst : in std_logic;
  clr : in std_logic;                        -- book-only clear (empties levels; inventory/params untouched)
  msg_in : in feed_msg_t; msg_valid : in std_logic;
  book_out : out book_out_t; book_valid : out std_logic ); end entity;

architecture rtl of order_book is

    type price_arr is array(0 to N_LEVELS-1) of tick_t;
    type qty_arr   is array(0 to N_LEVELS-1) of qty_t;

    constant PRICE_ZERO : tick_t := (others => '0');
    constant QTY_ZERO   : qty_t  := (others => '0');

    -- registered book storage, one set per side
    signal bid_p : price_arr := (others => PRICE_ZERO);
    signal bid_q : qty_arr   := (others => QTY_ZERO);
    signal ask_p : price_arr := (others => PRICE_ZERO);
    signal ask_q : qty_arr   := (others => QTY_ZERO);
    signal bid_n : integer range 0 to N_LEVELS := 0;
    signal ask_n : integer range 0 to N_LEVELS := 0;

    -- last valid mid held across empty-side gaps
    signal mid_hold : tick_t := (others => '0');

    -- registered outputs
    signal book_out_r   : book_out_t := (ts_ns => (others => '0'),
                                         mid    => (others => '0'),
                                         spread => (others => '0'),
                                         imbalance => (others => '0'));
    signal book_valid_r : std_logic := '0';

    -- STAGE 2 pipeline: the mid/spread/imbalance output is computed ONE cycle
    -- after the insert, from the now-registered book (bid_p/ask_p index 0).
    -- This breaks the long "insert -> select best -> mid" combinational path so
    -- the design closes 250 MHz timing. valid is delayed one cycle to match.
    signal emit_pending : std_logic := '0';
    signal emit_ts      : ts_t := (others => '0');

begin

    book_out   <= book_out_r;
    book_valid <= book_valid_r;

    process(clk)
        -- working copies of the touched side
        variable vp  : price_arr;
        variable vq  : qty_arr;
        variable vn  : integer range 0 to N_LEVELS;
        variable asc : boolean;                 -- true for ask (ascending sort)

        variable found     : boolean;
        variable fidx      : integer range 0 to N_LEVELS-1;
        variable ins_found : boolean;
        variable ins       : integer range 0 to N_LEVELS;
        variable do_remove : boolean;

        -- output math
        variable best_bid, best_ask : tick_t;
        variable bidQ, askQ         : qty_t;
        variable new_mid, new_spr   : tick_t;
        variable sum_u              : unsigned(32 downto 0);
        variable diff_s             : signed(33 downto 0);
        variable msb                : integer range 0 to 32;
        variable scaled             : signed(49 downto 0);  -- diff<<15 then >>msb
        variable imb                : signed(15 downto 0);
    begin
        if rising_edge(clk) then
            if rst = '1' then
                bid_p <= (others => PRICE_ZERO);
                bid_q <= (others => QTY_ZERO);
                ask_p <= (others => PRICE_ZERO);
                ask_q <= (others => QTY_ZERO);
                bid_n <= 0;
                ask_n <= 0;
                mid_hold     <= (others => '0');
                book_valid_r <= '0';
                book_out_r   <= (ts_ns => (others => '0'),
                                 mid    => (others => '0'),
                                 spread => (others => '0'),
                                 imbalance => (others => '0'));
            else
                book_valid_r <= '0';
                emit_pending <= '0';

                if clr = '1' then
                    -- book-only clear: empty both sides (levels + counts). Inventory
                    -- and ParamBlock live in other modules and are NOT touched, so a
                    -- fresh 2-sided book can be rebuilt each tick while the on-chip
                    -- inventory (and thus the A-S skew) persists across ticks.
                    bid_p <= (others => PRICE_ZERO); bid_q <= (others => QTY_ZERO);
                    ask_p <= (others => PRICE_ZERO); ask_q <= (others => QTY_ZERO);
                    bid_n <= 0; ask_n <= 0;
                elsif msg_valid = '1' then
                    -- ---- load the side this message touches --------------------
                    if msg_in.side = BID then
                        vp := bid_p; vq := bid_q; vn := bid_n; asc := false;
                    else
                        vp := ask_p; vq := ask_q; vn := ask_n; asc := true;
                    end if;

                    -- ---- locate existing level at this price -------------------
                    found := false; fidx := 0;
                    for i in 0 to N_LEVELS-1 loop
                        if (not found) and (i < vn) and (vp(i) = msg_in.price) then
                            found := true; fidx := i;
                        end if;
                    end loop;

                    do_remove := false;

                    if msg_in.msg_type = MSG_ADD then
                        if found then
                            -- accumulate size onto the existing level
                            vq(fidx) := vq(fidx) + msg_in.size;
                        else
                            -- sorted insert: first slot the new price outranks
                            ins_found := false; ins := vn;
                            for i in 0 to N_LEVELS-1 loop
                                if (not ins_found) and (i < vn) and
                                   ((asc and (vp(i) > msg_in.price)) or
                                    ((not asc) and (vp(i) < msg_in.price))) then
                                    ins_found := true; ins := i;
                                end if;
                            end loop;
                            -- drop only if book is full AND price is worst
                            if (vn < N_LEVELS) or (ins < N_LEVELS) then
                                for i in N_LEVELS-1 downto 1 loop
                                    if i > ins then
                                        vp(i) := vp(i-1); vq(i) := vq(i-1);
                                    end if;
                                end loop;
                                vp(ins) := msg_in.price;
                                vq(ins) := msg_in.size;
                                if vn < N_LEVELS then
                                    vn := vn + 1;
                                end if;
                            end if;
                        end if;

                    elsif msg_in.msg_type = MSG_DEL then
                        if found then
                            do_remove := true;
                        end if;

                    elsif (msg_in.msg_type = MSG_EXEC) or
                          (msg_in.msg_type = MSG_TRADE) then
                        if found then
                            if vq(fidx) > msg_in.size then
                                vq(fidx) := vq(fidx) - msg_in.size;
                            else
                                do_remove := true;   -- fully consumed
                            end if;
                        end if;
                    end if;

                    -- ---- shared remove: shift levels up, clear the tail --------
                    if do_remove then
                        for i in 0 to N_LEVELS-2 loop
                            if i >= fidx then
                                vp(i) := vp(i+1); vq(i) := vq(i+1);
                            end if;
                        end loop;
                        vp(N_LEVELS-1) := PRICE_ZERO;
                        vq(N_LEVELS-1) := QTY_ZERO;
                        if vn > 0 then
                            vn := vn - 1;
                        end if;
                    end if;

                    -- ---- write the touched side back --------------------------
                    if msg_in.side = BID then
                        bid_p <= vp; bid_q <= vq; bid_n <= vn;
                    else
                        ask_p <= vp; ask_q <= vq; ask_n <= vn;
                    end if;

                    -- STAGE 1 done. Schedule the output for the NEXT cycle, when
                    -- bid_p/ask_p hold the just-inserted book (STAGE 2 below).
                    emit_pending <= '1';
                    emit_ts      <= msg_in.ts_ns;
                end if;

                -- ============ STAGE 2 ============ (1 cycle after the insert:
                -- mid/spread/imbalance from the registered book top -> short path)
                if emit_pending = '1' then
                    if bid_n > 0 then best_bid := bid_p(0); bidQ := bid_q(0);
                    else             best_bid := PRICE_ZERO; bidQ := QTY_ZERO; end if;
                    if ask_n > 0 then best_ask := ask_p(0); askQ := ask_q(0);
                    else             best_ask := PRICE_ZERO; askQ := QTY_ZERO; end if;

                    if (bid_n > 0) and (ask_n > 0) then
                        new_mid := mid_of(best_bid, best_ask);
                        new_spr := best_ask - best_bid;
                    else
                        new_mid := mid_hold;
                        new_spr := (others => '0');
                    end if;
                    mid_hold <= new_mid;

                    sum_u  := resize(bidQ, 33) + resize(askQ, 33);
                    diff_s := resize(signed('0' & bidQ), 34) -
                              resize(signed('0' & askQ), 34);
                    if sum_u = 0 then
                        imb := (others => '0');
                    else
                        msb := 0;
                        for i in 0 to 32 loop
                            if sum_u(i) = '1' then msb := i; end if;
                        end loop;
                        scaled := shift_right(shift_left(resize(diff_s, 50), 15), msb);
                        if scaled > to_signed(32767, 50) then imb := to_signed(32767, 16);
                        elsif scaled < to_signed(-32768, 50) then imb := to_signed(-32768, 16);
                        else imb := resize(scaled, 16); end if;
                    end if;

                    book_out_r.ts_ns     <= emit_ts;
                    book_out_r.mid       <= new_mid;
                    book_out_r.spread    <= new_spr;
                    book_out_r.imbalance <= imb;
                    book_valid_r         <= '1';
                end if;
            end if;
        end if;
    end process;

end architecture rtl;
