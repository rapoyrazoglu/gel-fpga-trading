-- tb_hmac — verifies hmac_verify against a HOST-generated HMAC-SHA256 tag
-- (host/ipc/hmac.cpp). key=0xab*32, data=0x00..0x2f, host TAG16=37439f6f...0c34c.
library ieee;
use ieee.std_logic_1164.all;

entity tb_hmac is end entity;

architecture sim of tb_hmac is
    signal clk : std_logic := '0';
    signal rst : std_logic := '1';
    signal start, done, ok : std_logic := '0';
    signal data : std_logic_vector(383 downto 0) :=
        x"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f";
    signal key  : std_logic_vector(255 downto 0) :=
        x"abababababababababababababababababababababababababababababababab";
    signal tag  : std_logic_vector(127 downto 0) := x"37439f6faa71849ae6598aff5880c34c";
begin
    clk <= not clk after 2 ns;

    dut : entity work.hmac_verify
        port map(clk=>clk, rst=>rst, start=>start, data=>data, key=>key, tag=>tag, done=>done, ok=>ok);

    process
    begin
        wait for 10 ns; rst <= '0';
        -- run 1: correct host tag -> expect ok=1
        wait until rising_edge(clk); start <= '1';
        wait until rising_edge(clk); start <= '0';
        wait until done = '1';
        if ok = '1' then report "PASS: valid host tag accepted (ok=1)" severity note;
        else report "FAIL: valid host tag REJECTED" severity failure; end if;
        wait for 20 ns;
        -- run 2: tamper one bit of the tag -> expect ok=0
        tag <= x"37439f6faa71849ae6598aff5880c34d";  -- last nibble c->d
        wait until rising_edge(clk); start <= '1';
        wait until rising_edge(clk); start <= '0';
        wait until done = '1';
        if ok = '0' then report "PASS: tampered tag rejected (ok=0)" severity note;
        else report "FAIL: tampered tag ACCEPTED" severity failure; end if;
        wait for 10 ns;
        std.env.stop;
    end process;
end architecture;
