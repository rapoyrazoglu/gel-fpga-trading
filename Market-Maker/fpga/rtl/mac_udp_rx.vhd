-- MMX — fpga/rtl/mac_udp_rx.vhd
-- Spec §4.1 line-rate ingress (Phase-B data plane).
--
-- Sits downstream of a Xilinx 25G MAC (external IP) exposing a 64-bit AXI-Stream
-- (mac_t*).  This block strips the Ethernet/IP/UDP headers and forwards ONLY the
-- UDP payload of frames whose UDP destination port == cfg_udp_port, as a clean,
-- lane-0 aligned AXI-Stream (pl_t*).  Non-IPv4, non-UDP, or wrong-port frames are
-- dropped (silently consumed).  Latency over throughput: no FIFO, only the small
-- 6-byte realignment register the header-strip arithmetically requires.
--
-- Frame layout assumed (no VLAN, no IP options):
--   Ethernet  14B : dst[6] src[6] ethertype[2]            (ethertype @ byte 12..13)
--   IPv4      20B : ... protocol @ byte 23 ...             (UDP = 0x11)
--   UDP        8B : srcport[2] dstport[2] len[2] csum[2]   (dstport @ byte 36..37)
--   Payload   42B header total -> payload starts at byte 42
--
-- AXIS byte/lane convention: lane i = tdata((8*i+7) downto 8*i), lane 0 first on
-- the wire.  42 is not a multiple of 8, so the payload is offset by 2 bytes inside
-- the beats; we realign with a 6-byte residual register so pl_t* is lane-0 clean.
--
-- VHDL-2008, single clock 250 MHz, synchronous active-high reset, numeric_std only.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity mac_udp_rx is port( clk, rst : in std_logic;
  mac_tdata : in std_logic_vector(63 downto 0); mac_tkeep : in std_logic_vector(7 downto 0);
  mac_tvalid : in std_logic; mac_tlast : in std_logic; mac_tready : out std_logic;
  cfg_udp_port : in std_logic_vector(15 downto 0);
  pl_tdata : out std_logic_vector(63 downto 0); pl_tkeep : out std_logic_vector(7 downto 0);
  pl_tvalid : out std_logic; pl_tlast : out std_logic; pl_tready : in std_logic ); end entity;

architecture rtl of mac_udp_rx is

  type state_t is (S_HDR, S_PAY, S_FLUSH, S_DROP);
  signal state    : state_t := S_HDR;
  signal bcnt     : unsigned(2 downto 0) := (others => '0'); -- header beat index 0..5
  signal first_pay: std_logic := '0';                        -- next payload beat is beat 5

  -- header-check results captured during the header beats
  signal eth_ok   : std_logic := '0';   -- ethertype == 0x0800 (IPv4)
  signal ip_ok    : std_logic := '0';   -- IP protocol == 0x11 (UDP)

  -- realignment residual: up to 6 payload bytes carried between beats (res(7:0)=byte0)
  signal res      : std_logic_vector(47 downto 0) := (others => '0');
  signal flush_d  : std_logic_vector(47 downto 0) := (others => '0');
  signal flush_n  : integer range 0 to 6 := 0;

  -- registered output (one-deep skid)
  signal r_tdata  : std_logic_vector(63 downto 0) := (others => '0');
  signal r_tkeep  : std_logic_vector(7 downto 0)  := (others => '0');
  signal r_tvalid : std_logic := '0';
  signal r_tlast  : std_logic := '0';

  signal can_load : std_logic;

  -- count contiguous valid byte-lanes from tkeep (0..8)
  function count_lanes(k : std_logic_vector(7 downto 0)) return integer is
    variable n : integer range 0 to 8 := 0;
  begin
    for i in 0 to 7 loop
      if k(i) = '1' then n := n + 1; end if;
    end loop;
    return n;
  end function;

  -- byte mask with the low n lanes set
  function keep_mask(n : integer) return std_logic_vector is
    variable m : std_logic_vector(7 downto 0) := (others => '0');
  begin
    for i in 0 to 7 loop
      if i < n then m(i) := '1'; end if;
    end loop;
    return m;
  end function;

begin

  -- accept a new input beat when the output register is free or being drained
  can_load <= '1' when (r_tvalid = '0' or pl_tready = '1') else '0';

  -- backpressure to the MAC: hold input while flushing the trailing beat
  mac_tready <= '0' when state = S_FLUSH else can_load;

  -- drive the AXIS output ports from the registered skid
  pl_tdata  <= r_tdata;
  pl_tkeep  <= r_tkeep;
  pl_tvalid <= r_tvalid;
  pl_tlast  <= r_tlast;

  process(clk)
    variable lanes    : integer range 0 to 8;
    variable in_bytes : integer range 0 to 8;
    variable pay_bytes: integer range 0 to 6;
    variable total    : integer range 0 to 14;
    variable fwd      : std_logic;
  begin
    if rising_edge(clk) then
      if rst = '1' then
        state    <= S_HDR;
        bcnt     <= (others => '0');
        first_pay<= '0';
        eth_ok   <= '0';
        ip_ok    <= '0';
        res      <= (others => '0');
        flush_d  <= (others => '0');
        flush_n  <= 0;
        r_tvalid <= '0';
        r_tlast  <= '0';
        r_tdata  <= (others => '0');
        r_tkeep  <= (others => '0');
      else
        -- drain the output register when consumed (may be re-written below)
        if r_tvalid = '1' and pl_tready = '1' then
          r_tvalid <= '0';
        end if;

        case state is

          -- ---- header strip ------------------------------------------------
          when S_HDR =>
            if mac_tvalid = '1' and can_load = '1' then
              if mac_tlast = '1' then
                -- runt / malformed frame before the full 42B header: drop it
                state <= S_HDR;
                bcnt  <= (others => '0');
              else
                case to_integer(bcnt) is
                  when 0 =>            -- bytes 0..7 (dst/src MAC); reset checks
                    eth_ok <= '0';
                    ip_ok  <= '0';
                  when 1 =>            -- bytes 8..15: ethertype @ 12..13
                    if mac_tdata(39 downto 32) = x"08" and
                       mac_tdata(47 downto 40) = x"00" then
                      eth_ok <= '1';
                    else
                      eth_ok <= '0';
                    end if;
                  when 2 =>            -- bytes 16..23: IP protocol @ byte 23
                    if mac_tdata(63 downto 56) = x"11" then
                      ip_ok <= '1';
                    else
                      ip_ok <= '0';
                    end if;
                  when 4 =>            -- bytes 32..39: UDP dst port @ 36..37
                    fwd := eth_ok and ip_ok;
                    if mac_tdata(39 downto 32) = cfg_udp_port(15 downto 8) and
                       mac_tdata(47 downto 40) = cfg_udp_port(7 downto 0) then
                      null;
                    else
                      fwd := '0';
                    end if;
                    if fwd = '1' then
                      state     <= S_PAY;
                      first_pay <= '1';
                    else
                      state <= S_DROP;
                    end if;
                  when others =>       -- beat 3 (bytes 24..31): nothing to capture
                    null;
                end case;
                bcnt <= bcnt + 1;
              end if;
            end if;

          -- ---- payload forward with 2-byte realignment ---------------------
          when S_PAY =>
            if mac_tvalid = '1' and can_load = '1' then
              if mac_tlast = '1' then
                lanes := count_lanes(mac_tkeep);
              else
                lanes := 8;
              end if;

              if first_pay = '1' then
                -- beat 5: payload occupies lanes 2..7 (bytes 42..47), residual empty
                if lanes > 2 then
                  pay_bytes := lanes - 2;
                else
                  pay_bytes := 0;
                end if;
                first_pay <= '0';
                if mac_tlast = '1' then
                  if pay_bytes > 0 then
                    r_tdata  <= x"0000" & mac_tdata(63 downto 16);
                    r_tkeep  <= keep_mask(pay_bytes);
                    r_tlast  <= '1';
                    r_tvalid <= '1';
                  end if;
                  state <= S_HDR;
                  bcnt  <= (others => '0');
                else
                  res <= mac_tdata(63 downto 16);   -- carry 6 bytes
                end if;

              else
                -- steady state: residual holds 6 bytes; emit res ++ in(low 2 bytes)
                in_bytes := lanes;
                total    := 6 + in_bytes;
                if mac_tlast = '0' then
                  r_tdata  <= mac_tdata(15 downto 0) & res;
                  r_tkeep  <= (others => '1');
                  r_tlast  <= '0';
                  r_tvalid <= '1';
                  res      <= mac_tdata(63 downto 16);
                else
                  if total <= 8 then
                    r_tdata  <= mac_tdata(15 downto 0) & res;
                    r_tkeep  <= keep_mask(total);
                    r_tlast  <= '1';
                    r_tvalid <= '1';
                    state <= S_HDR;
                    bcnt  <= (others => '0');
                  else
                    -- two output beats: emit 8 now, flush the rest next cycle
                    r_tdata  <= mac_tdata(15 downto 0) & res;
                    r_tkeep  <= (others => '1');
                    r_tlast  <= '0';
                    r_tvalid <= '1';
                    flush_d  <= mac_tdata(63 downto 16);
                    flush_n  <= in_bytes - 2;
                    state    <= S_FLUSH;
                  end if;
                end if;
              end if;
            end if;

          -- ---- trailing realignment beat -----------------------------------
          when S_FLUSH =>
            if r_tvalid = '0' or pl_tready = '1' then
              r_tdata  <= x"0000" & flush_d;
              r_tkeep  <= keep_mask(flush_n);
              r_tlast  <= '1';
              r_tvalid <= '1';
              state    <= S_HDR;
              bcnt     <= (others => '0');
            end if;

          -- ---- drop non-matching frame -------------------------------------
          when S_DROP =>
            if mac_tvalid = '1' and can_load = '1' then
              if mac_tlast = '1' then
                state <= S_HDR;
                bcnt  <= (others => '0');
              end if;
            end if;

        end case;
      end if;
    end if;
  end process;

end architecture rtl;
