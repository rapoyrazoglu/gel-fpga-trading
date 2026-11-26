-- MMX — fpga/rtl/sha256_core.vhd
-- FIPS 180-4 SHA-256, synthesizable, one round per cycle (64 cycles/block).
-- Mirrors host/ipc/sha256.cpp so the FPGA HMAC matches the host bit-for-bit.
--
-- Usage: pulse `init` to load the IV; then for each 512-bit message block pulse
-- `block_vld` with `block_in` (big-endian: block_in(511..504)=byte0=W0 MSB).
-- `block_done` pulses when the block is absorbed; `digest` holds H0..H7 with
-- H0 in bits 255..224. Caller does the padding (append 0x80 + 64-bit bit-length).

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity sha256_core is
    port(
        clk, rst   : in  std_logic;
        init       : in  std_logic;                       -- reset state to IV
        block_in   : in  std_logic_vector(511 downto 0);  -- one 512-bit block
        block_vld  : in  std_logic;                       -- absorb block_in
        busy       : out std_logic;
        block_done : out std_logic;                       -- 1-cycle pulse
        digest     : out std_logic_vector(255 downto 0)
    );
end entity;

architecture rtl of sha256_core is

    type w32_arr is array(natural range <>) of unsigned(31 downto 0);

    constant IV : w32_arr(0 to 7) := (
        x"6a09e667", x"bb67ae85", x"3c6ef372", x"a54ff53a",
        x"510e527f", x"9b05688c", x"1f83d9ab", x"5be0cd19");

    constant K : w32_arr(0 to 63) := (
        x"428a2f98", x"71374491", x"b5c0fbcf", x"e9b5dba5", x"3956c25b", x"59f111f1", x"923f82a4", x"ab1c5ed5",
        x"d807aa98", x"12835b01", x"243185be", x"550c7dc3", x"72be5d74", x"80deb1fe", x"9bdc06a7", x"c19bf174",
        x"e49b69c1", x"efbe4786", x"0fc19dc6", x"240ca1cc", x"2de92c6f", x"4a7484aa", x"5cb0a9dc", x"76f988da",
        x"983e5152", x"a831c66d", x"b00327c8", x"bf597fc7", x"c6e00bf3", x"d5a79147", x"06ca6351", x"14292967",
        x"27b70a85", x"2e1b2138", x"4d2c6dfc", x"53380d13", x"650a7354", x"766a0abb", x"81c2c92e", x"92722c85",
        x"a2bfe8a1", x"a81a664b", x"c24b8b70", x"c76c51a3", x"d192e819", x"d6990624", x"f40e3585", x"106aa070",
        x"19a4c116", x"1e376c08", x"2748774c", x"34b0bcb5", x"391c0cb3", x"4ed8aa4a", x"5b9cca4f", x"682e6ff3",
        x"748f82ee", x"78a5636f", x"84c87814", x"8cc70208", x"90befffa", x"a4506ceb", x"bef9a3f7", x"c67178f2");

    function rotr(x : unsigned(31 downto 0); n : natural) return unsigned is
    begin
        return rotate_right(x, n);
    end function;

    function bsig0(x : unsigned(31 downto 0)) return unsigned is
    begin return rotr(x,2) xor rotr(x,13) xor rotr(x,22); end function;
    function bsig1(x : unsigned(31 downto 0)) return unsigned is
    begin return rotr(x,6) xor rotr(x,11) xor rotr(x,25); end function;
    function ssig0(x : unsigned(31 downto 0)) return unsigned is
    begin return rotr(x,7) xor rotr(x,18) xor shift_right(x,3); end function;
    function ssig1(x : unsigned(31 downto 0)) return unsigned is
    begin return rotr(x,17) xor rotr(x,19) xor shift_right(x,10); end function;
    function ch(e,f,g : unsigned(31 downto 0)) return unsigned is
    begin return (e and f) xor ((not e) and g); end function;
    function maj(a,b,c : unsigned(31 downto 0)) return unsigned is
    begin return (a and b) xor (a and c) xor (b and c); end function;

    type state_t is (S_IDLE, S_ROUND, S_FINAL);
    signal st   : state_t := S_IDLE;
    signal hs    : w32_arr(0 to 7) := IV;
    signal a,b,c,d,e,f,g,h : unsigned(31 downto 0) := (others=>'0');
    signal w    : w32_arr(0 to 15) := (others=>(others=>'0'));
    signal t    : integer range 0 to 63 := 0;

begin

    busy <= '0' when st = S_IDLE else '1';

    process(clk)
        variable wt, s0, s1, t1, t2, neww : unsigned(31 downto 0);
        variable i2, i7, i15, i16 : integer range 0 to 15;
    begin
        if rising_edge(clk) then
            block_done <= '0';
            if rst = '1' then
                st <= S_IDLE; hs <= IV;
            elsif init = '1' then
                hs <= IV;
            elsif st = S_IDLE and block_vld = '1' then
                -- latch the 16 big-endian message words, seed working vars
                for j in 0 to 15 loop
                    w(j) <= unsigned(block_in(511-32*j downto 480-32*j));
                end loop;
                a <= hs(0); b <= hs(1); c <= hs(2); d <= hs(3);
                e <= hs(4); f <= hs(5); g <= hs(6); h <= hs(7);
                t <= 0; st <= S_ROUND;
            elsif st = S_ROUND then
                if t < 16 then
                    wt := w(t);
                else
                    i16 := t mod 16;
                    i15 := (t+1) mod 16;
                    i7  := (t+9) mod 16;
                    i2  := (t+14) mod 16;
                    neww := ssig1(w(i2)) + w(i7) + ssig0(w(i15)) + w(i16);
                    w(i16) <= neww;
                    wt := neww;
                end if;
                s1 := bsig1(e);
                s0 := bsig0(a);
                t1 := h + s1 + ch(e,f,g) + K(t) + wt;
                t2 := s0 + maj(a,b,c);
                h <= g; g <= f; f <= e; e <= d + t1;
                d <= c; c <= b; b <= a; a <= t1 + t2;
                if t = 63 then
                    st <= S_FINAL;
                else
                    t <= t + 1;
                end if;
            elsif st = S_FINAL then
                hs(0) <= hs(0) + a; hs(1) <= hs(1) + b; hs(2) <= hs(2) + c; hs(3) <= hs(3) + d;
                hs(4) <= hs(4) + e; hs(5) <= hs(5) + f; hs(6) <= hs(6) + g; hs(7) <= hs(7) + h;
                block_done <= '1';
                st <= S_IDLE;
            end if;
        end if;
    end process;

    gen_dig : for j in 0 to 7 generate
        digest(255-32*j downto 224-32*j) <= std_logic_vector(hs(j));
    end generate;

end architecture;
