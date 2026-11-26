-- tb_pricing_parity — proves pricing_engine_beh == host compute_quote() bit-for-bit.
-- Reads pricing_vectors.txt (generated from the C++ compute_quote over 500 random
-- (s,q,ParamBlock) cases) and checks the RTL output matches every expected quote.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use std.textio.all;
use work.mmx_pkg.all;

entity tb_pricing_parity_l2 is end entity;

architecture sim of tb_pricing_parity_l2 is
    signal clk : std_logic := '0';
    signal rst : std_logic := '1';
    signal s_in, q_in : tick_t := (others=>'0');
    signal valid_in   : std_logic := '0';
    signal pblk       : param_block_t := PARAM_BLOCK_INIT;
    signal quote_out  : quote_t;
    signal quote_valid: std_logic;
begin
    clk <= not clk after 2 ns;

    dut : entity work.pricing_engine_beh_buggy
        port map(clk=>clk, rst=>rst, s_in=>s_in, q_in=>q_in, valid_in=>valid_in,
                 pblk=>pblk, quote_out=>quote_out, quote_valid=>quote_valid);

    process
        file vf : text open read_mode is "pricing_vectors.txt";
        variable L : line;
        variable n, s, q, inv, hs, mx, bq, bsk, ask_sk, kl, ebid, eask, eqty, ecan : integer;
        variable p : param_block_t := PARAM_BLOCK_INIT;
        variable mism : integer := 0;
    begin
        wait for 10 ns; rst <= '0';
        readline(vf, L); read(L, n);
        for i in 1 to n loop
            readline(vf, L);
            read(L,s); read(L,q); read(L,inv); read(L,hs); read(L,mx); read(L,bq);
            read(L,bsk); read(L,ask_sk); read(L,kl); read(L,ebid); read(L,eask);
            read(L,eqty); read(L,ecan);

            p.inv_skew_q31  := to_signed(inv,32);
            p.half_spread   := to_signed(hs,32);
            p.max_inv       := to_unsigned(mx,32);
            p.base_qty      := to_unsigned(bq,32);
            p.bid_skew      := to_signed(bsk,16);
            p.ask_skew      := to_signed(ask_sk,16);
            if kl = 1 then p.kill_switch := '1'; else p.kill_switch := '0'; end if;
            p.valid := '1';

            wait until rising_edge(clk);
            s_in <= to_signed(s,32); q_in <= to_signed(q,32); pblk <= p; valid_in <= '1';
            wait until rising_edge(clk);
            valid_in <= '0';
            wait until quote_valid = '1';

            if to_integer(quote_out.bid) /= ebid
               or to_integer(quote_out.ask) /= eask
               or to_integer(quote_out.qty) /= eqty
               or (quote_out.cancel_only = '1') /= (ecan = 1) then
                mism := mism + 1;
                if mism <= 5 then
                    report "MISMATCH vec " & integer'image(i)
                        & " got bid=" & integer'image(to_integer(quote_out.bid))
                        & " ask=" & integer'image(to_integer(quote_out.ask))
                        & " exp bid=" & integer'image(ebid) & " ask=" & integer'image(eask)
                        severity warning;
                end if;
            end if;
        end loop;

        if mism > 0 then
            report "PASS (L2 tester's test): injected bug CAUGHT -- parity flagged "
                 & integer'image(mism) & "/" & integer'image(n)
                 & " mismatches, so the L1 parity test is load-bearing (not vacuous)" severity note;
        else
            report "FAIL (L2): injected bug NOT caught -- the parity test proves nothing!"
                 severity failure;
        end if;
        wait for 10 ns;
        std.env.stop;
    end process;
end architecture;
