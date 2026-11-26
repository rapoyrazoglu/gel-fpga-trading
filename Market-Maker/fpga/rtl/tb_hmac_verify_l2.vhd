-- tb_hmac_verify_l2 — L2 tester's-test for hmac_verify.
-- Drives the DELIBERATELY BUGGY hmac_verify_buggy exactly like tb_hmac drives
-- hmac_verify (host-generated HMAC-SHA256 vector: key=0xab*32, data=0x00..0x2f,
-- host TAG16=37439f6f...0c34c), applies the same L1-style check, and asserts
-- that the check CATCHES the injected bug. The L1 tb requires a VALID host tag
-- to be ACCEPTED (ok=1). For the buggy DUT the inner length constant is wrong,
-- so the recomputed MAC differs and the valid tag is REJECTED (ok=0).
-- This L2 test reports "PASS (L2): injected bug CAUGHT" when ok=0 on the valid
-- tag. If the buggy DUT still accepted the valid tag (ok=1), the L1 check would
-- be vacuous and this L2 test reports FAIL.
library ieee;
use ieee.std_logic_1164.all;

entity tb_hmac_verify_l2 is end entity;

architecture sim of tb_hmac_verify_l2 is
    signal clk : std_logic := '0';
    signal rst : std_logic := '1';
    signal start, done, ok : std_logic := '0';
    signal data : std_logic_vector(383 downto 0) :=
        x"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f";
    signal key  : std_logic_vector(255 downto 0) :=
        x"abababababababababababababababababababababababababababababababab";
    -- Correct host tag (the L1 tb expects this to be ACCEPTED by the good DUT).
    signal tag  : std_logic_vector(127 downto 0) := x"37439f6faa71849ae6598aff5880c34c";
begin
    clk <= not clk after 2 ns;

    dut : entity work.hmac_verify_buggy
        port map(clk=>clk, rst=>rst, start=>start, data=>data, key=>key, tag=>tag, done=>done, ok=>ok);

    process
    begin
        wait for 10 ns; rst <= '0';
        -- Present the VALID host tag; the good DUT would accept it (ok=1).
        wait until rising_edge(clk); start <= '1';
        wait until rising_edge(clk); start <= '0';
        wait until done = '1';
        -- L1-style check, inverted for L2: the bug is CAUGHT iff the valid tag
        -- is REJECTED (ok=0) by the buggy MAC computation.
        if ok = '0' then
            report "PASS (L2): injected bug CAUGHT - buggy hmac_verify REJECTED the valid host tag (ok=0)"
                   severity note;
        else
            report "FAIL (L2): buggy hmac_verify accepted the valid tag - L1 check is vacuous"
                   severity failure;
        end if;
        wait for 10 ns;
        std.env.stop;
    end process;
end architecture;
