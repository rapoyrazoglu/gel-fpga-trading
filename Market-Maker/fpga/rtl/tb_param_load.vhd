-- tb_param_load — feeds param_block_loader a HOST-SIGNED ParamBlock (16 words
-- dumped from host sign_param_block, key=0xab*32) and checks the real HMAC core
-- authenticates it: load_err=00, kill=0, valid=1, half_spread=5. Then tampers a
-- data word and checks the loader rejects it (HMAC fail -> kill).
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity tb_param_load is end entity;

architecture sim of tb_param_load is
    signal clk : std_logic := '0';
    signal rst : std_logic := '1';
    signal wr_en, commit : std_logic := '0';
    signal wr_addr : std_logic_vector(3 downto 0) := (others=>'0');
    signal wr_data : std_logic_vector(31 downto 0) := (others=>'0');
    signal now_ns  : ts_t := (others=>'0');
    signal pblk    : param_block_t;
    signal err     : std_logic_vector(1 downto 0);

    type warr is array(0 to 15) of std_logic_vector(31 downto 0);
    -- host-signed block: half_spread=5, max_inv=100, base_qty=10, requote=1, kill=0
    constant W : warr := (
        x"00000000", x"00000005", x"00000064", x"0000000a",
        x"00000000", x"00010000", x"00000000", x"00000000",
        x"00000000", x"00000000", x"00000000", x"00000000",
        x"b55305c9", x"34530487", x"88fbee6a", x"2d969112");

    procedure load(signal en : out std_logic; signal ad : out std_logic_vector(3 downto 0);
                   signal da : out std_logic_vector(31 downto 0);
                   signal cm : out std_logic; signal clk_s : in std_logic;
                   blk : warr) is
    begin
        for i in 0 to 15 loop
            wait until rising_edge(clk_s);
            en <= '1'; ad <= std_logic_vector(to_unsigned(i,4)); da <= blk(i);
        end loop;
        wait until rising_edge(clk_s); en <= '0';
        wait until rising_edge(clk_s); cm <= '1';
        wait until rising_edge(clk_s); cm <= '0';
    end procedure;
begin
    clk <= not clk after 2 ns;

    dut : entity work.param_block_loader
        port map(clk=>clk, rst=>rst, s_wr_en=>wr_en, s_wr_addr=>wr_addr,
                 s_wr_data=>wr_data, s_commit=>commit, now_ns=>now_ns,
                 pblk_out=>pblk, load_err=>err);

    process
        variable Wt : warr;
    begin
        wait for 10 ns; rst <= '0';
        -- run 1: valid host-signed block
        load(wr_en, wr_addr, wr_data, commit, clk, W);
        wait until pblk.valid = '1' for 5 us;
        if pblk.valid='1' and err="00" and pblk.kill_switch='0'
           and pblk.half_spread = to_signed(5,32) then
            report "PASS: host-signed block authenticated (err=00 kill=0 half_spread=5)" severity note;
        else
            report "FAIL: valid block rejected/miswired (valid=" & std_logic'image(pblk.valid)
                 & " err=" & to_hstring(err) & ")" severity failure;
        end if;
        wait for 50 ns;
        -- run 2: tamper one data word -> HMAC must fail -> kill
        Wt := W; Wt(2) := x"00000063";  -- max_inv 100 -> 99, breaks the tag
        load(wr_en, wr_addr, wr_data, commit, clk, Wt);
        wait for 1500 ns;  -- let HMAC finish
        if err="01" and pblk.kill_switch='1' then
            report "PASS: tampered block rejected (err=01 HMAC-fail kill)" severity note;
        else
            report "FAIL: tampered block not rejected (err=" & to_hstring(err)
                 & " kill=" & std_logic'image(pblk.kill_switch) & ")" severity failure;
        end if;
        wait for 20 ns;
        std.env.stop;
    end process;
end architecture;
