-- tb_sha256 — checks sha256_core against the FIPS test vector SHA256("abc").
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity tb_sha256 is end entity;

architecture sim of tb_sha256 is
    signal clk : std_logic := '0';
    signal rst : std_logic := '1';
    signal init, block_vld, busy, block_done : std_logic := '0';
    signal block_in : std_logic_vector(511 downto 0) := (others=>'0');
    signal digest   : std_logic_vector(255 downto 0);

    constant EXP : std_logic_vector(255 downto 0) :=
        x"ba7816bf_8f01cfea_414140de_5dae2223_b00361a3_96177a9c_b410ff61_f20015ad";
begin
    clk <= not clk after 2 ns;

    dut : entity work.sha256_core
        port map(clk=>clk, rst=>rst, init=>init, block_in=>block_in,
                 block_vld=>block_vld, busy=>busy, block_done=>block_done, digest=>digest);

    process
    begin
        -- "abc" padded into one 512-bit block: 0x61626380, 0..0, len=0x18
        block_in <= (others=>'0');
        block_in(511 downto 480) <= x"61626380";
        block_in(31 downto 0)    <= x"00000018";
        wait for 10 ns; rst <= '0';
        wait until rising_edge(clk); init <= '1';
        wait until rising_edge(clk); init <= '0';
        wait until rising_edge(clk); block_vld <= '1';
        wait until rising_edge(clk); block_vld <= '0';
        wait until block_done = '1';
        wait until rising_edge(clk);
        if digest = EXP then
            report "PASS: SHA256(abc) = " & to_hstring(digest) severity note;
        else
            report "FAIL: got " & to_hstring(digest) & " exp " & to_hstring(EXP) severity failure;
        end if;
        wait for 10 ns;
        std.env.stop;
    end process;
end architecture;
