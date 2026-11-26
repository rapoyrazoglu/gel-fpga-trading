-- MMX — fpga/rtl/tb_param_block_loader_l2.vhd
-- L2 "tester's test" for param_block_loader: instantiates param_block_loader_buggy
-- (half_spread decoded from the wrong staging word — snap(0) instead of snap(1))
-- and drives it exactly the way the L1 testbench (tb_param_load) drives the real
-- loader: it writes the same HOST-SIGNED 16-word ParamBlock and commits.
--
-- The injected bug does NOT touch the HMAC data path (the tag covers the raw snap
-- words), so the buggy loader still authenticates the block: valid=1, err=00,
-- kill=0. But the decoded half_spread field is wrong. This TB applies the same
-- L1-style field check the real testbench applies ("half_spread=5") and reports
-- PASS when that check FAILS on the buggy DUT — i.e. the bug is DETECTED, proving
-- the L1 field check is load-bearing (not vacuous).

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity tb_param_block_loader_l2 is end entity;

architecture sim of tb_param_block_loader_l2 is
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

    dut : entity work.param_block_loader_buggy
        port map(clk=>clk, rst=>rst, s_wr_en=>wr_en, s_wr_addr=>wr_addr,
                 s_wr_data=>wr_data, s_commit=>commit, now_ns=>now_ns,
                 pblk_out=>pblk, load_err=>err);

    process
        variable mism : integer := 0;
    begin
        wait for 10 ns; rst <= '0';
        -- Drive the valid host-signed block exactly like the L1 testbench.
        load(wr_en, wr_addr, wr_data, commit, clk, W);
        wait until pblk.valid = '1' for 5 us;

        -- Same L1-style field check the real tb_param_load applies on the loaded
        -- block. On the good loader this passes (half_spread=5); on the buggy
        -- loader half_spread is decoded from snap(0)=0, so the check must FAIL.
        if pblk.half_spread /= to_signed(5,32) then
            mism := mism + 1;
        end if;

        if mism > 0 then
            report "PASS (L2 tester's test): injected bug CAUGHT -- param_block_loader "
                 & "half_spread field check flagged a mismatch (got "
                 & integer'image(to_integer(pblk.half_spread)) & ", expected 5), so the "
                 & "L1 field check is load-bearing (not vacuous)" severity note;
        else
            report "FAIL (L2): injected bug NOT caught -- the param_block_loader field "
                 & "check proves nothing!" severity failure;
        end if;

        wait for 20 ns;
        std.env.stop;
    end process;
end architecture;
