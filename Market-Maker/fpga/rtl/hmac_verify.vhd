-- MMX — fpga/rtl/hmac_verify.vhd
-- REAL HMAC-SHA256 verifier (replaces the Phase-B placeholder).
-- HMAC(key, data) = SHA256( (K' xor opad) || SHA256( (K' xor ipad) || data ) )
-- with ipad=0x36, opad=0x5c, K' = key zero-padded to 64 bytes (RFC 2104).
-- Mirrors host/ipc/hmac.cpp + sha256.cpp so the FPGA accepts exactly the blocks
-- the host signs. data = 48-byte signed region; tag = leading 16 bytes of the MAC.
--
-- Sizes are fixed: inner msg = 64(ipad)+48(data) = 112 B -> 2 SHA blocks;
-- outer msg = 64(opad)+32(inner) = 96 B -> 2 SHA blocks. So 4 blocks total.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity hmac_verify is
    port(
        clk, rst : in  std_logic;
        start    : in  std_logic;
        data     : in  std_logic_vector(383 downto 0);  -- 48 bytes, byte0 = MSB
        key      : in  std_logic_vector(255 downto 0);  -- 32 bytes, byte0 = MSB
        tag      : in  std_logic_vector(127 downto 0);  -- expected 16-byte tag
        done     : out std_logic;
        ok       : out std_logic
    );
end entity;

architecture rtl of hmac_verify is

    -- 32 bytes of 0x36 / 0x5c (built from 4x8-byte chunks to avoid miscount).
    constant IPAD32 : std_logic_vector(255 downto 0) :=
        x"3636363636363636" & x"3636363636363636" & x"3636363636363636" & x"3636363636363636";
    constant OPAD32 : std_logic_vector(255 downto 0) :=
        x"5c5c5c5c5c5c5c5c" & x"5c5c5c5c5c5c5c5c" & x"5c5c5c5c5c5c5c5c" & x"5c5c5c5c5c5c5c5c";

    -- SHA core handshake
    signal sha_init  : std_logic := '0';
    signal sha_block : std_logic_vector(511 downto 0) := (others=>'0');
    signal sha_vld   : std_logic := '0';
    signal sha_busy  : std_logic;
    signal sha_done  : std_logic;
    signal sha_dig   : std_logic_vector(255 downto 0);

    signal ipad_blk, opad_blk : std_logic_vector(511 downto 0);
    signal inner_blk1, outer_blk1 : std_logic_vector(511 downto 0);
    signal inner_dig : std_logic_vector(255 downto 0) := (others=>'0');

    type st_t is (IDLE, I_INIT, I_B0, I_W0, I_B1, I_W1, O_INIT, O_B0, O_W0, O_B1, O_W1, FIN);
    signal st : st_t := IDLE;

    signal done_r, ok_r : std_logic := '0';

begin

    sha : entity work.sha256_core
        port map(clk=>clk, rst=>rst, init=>sha_init, block_in=>sha_block,
                 block_vld=>sha_vld, busy=>sha_busy, block_done=>sha_done, digest=>sha_dig);

    -- ipad/opad blocks: high 32 bytes = key xor pad, low 32 bytes = pad (key zero-padded).
    ipad_blk <= (key xor IPAD32) & IPAD32;
    opad_blk <= (key xor OPAD32) & OPAD32;

    -- inner block 1 = data(48B) || 0x80 || zeros || len(112*8=896=0x380)
    inner_blk1 <= data & x"80" & (119 downto 64 => '0') & x"0000000000000380";
    -- outer block 1 = inner_dig(32B) || 0x80 || zeros || len(96*8=768=0x300)
    outer_blk1 <= inner_dig & x"80" & (247 downto 64 => '0') & x"0000000000000300";

    done <= done_r;
    ok   <= ok_r;

    process(clk)
    begin
        if rising_edge(clk) then
            sha_init <= '0'; sha_vld <= '0'; done_r <= '0';
            if rst = '1' then
                st <= IDLE; ok_r <= '0';
            else
                case st is
                    when IDLE =>
                        if start = '1' then st <= I_INIT; end if;
                    when I_INIT =>
                        sha_init <= '1'; st <= I_B0;
                    when I_B0 =>
                        sha_block <= ipad_blk; sha_vld <= '1'; st <= I_W0;
                    when I_W0 =>
                        if sha_done = '1' then st <= I_B1; end if;
                    when I_B1 =>
                        sha_block <= inner_blk1; sha_vld <= '1'; st <= I_W1;
                    when I_W1 =>
                        if sha_done = '1' then
                            inner_dig <= sha_dig;  -- capture inner hash
                            st <= O_INIT;
                        end if;
                    when O_INIT =>
                        sha_init <= '1'; st <= O_B0;
                    when O_B0 =>
                        sha_block <= opad_blk; sha_vld <= '1'; st <= O_W0;
                    when O_W0 =>
                        if sha_done = '1' then st <= O_B1; end if;
                    when O_B1 =>
                        sha_block <= outer_blk1; sha_vld <= '1'; st <= O_W1;
                    when O_W1 =>
                        if sha_done = '1' then st <= FIN; end if;
                    when FIN =>
                        -- truncated tag = leading 16 bytes of the MAC.
                        if sha_dig(255 downto 128) = tag then ok_r <= '1';
                        else ok_r <= '0'; end if;
                        done_r <= '1';
                        st <= IDLE;
                end case;
            end if;
        end if;
    end process;

end architecture;
