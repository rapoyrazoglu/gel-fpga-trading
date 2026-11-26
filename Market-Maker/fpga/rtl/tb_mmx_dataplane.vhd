-- MMX — fpga/rtl/tb_mmx_dataplane.vhd
-- Self-checking testbench for the Phase-B data plane top (mmx_dataplane).
--
-- Stimulus:
--   1. Hold reset, then release.
--   2. Write a valid 64-byte ParamBlock through the AXI-Lite-ish word port:
--        half_spread = 5, max_inv = 100, base_qty = 10, requote_ticks = 1,
--        kill_switch = 0, inv_skew = 0, epoch_ns = 0  (HMAC stub always OK).
--      Pulse s_commit and wait for the loader (hmac latency + load) to swap it in.
--   3. Drive a few ITCH-lite messages as FULL Ethernet/IPv4/UDP frames into the
--      RX MAC AXIS (mac_udp_rx strips the 42-byte header and forwards payload):
--        ADD  BID  ticker 1, price 100, size 50
--        ADD  ASK  ticker 1, price 110, size 50   -> book mid = 105
--        EXEC BID  ticker 1, price 100, size 10   -> our fill, inventory q += 10
--   4. With m_tready high, monitor the TX AXIS. Capture beat 2 of each emitted
--      frame (= bid:ask). Assert that within N cycles at least one TX frame is
--      produced (m_tvalid seen) AND the quote satisfies bid < ask.
--
-- Reports PASS/FAIL with assert/report. VHDL-2008.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity tb_mmx_dataplane is
end entity;

architecture sim of tb_mmx_dataplane is

  constant TCLK : time := 4 ns;   -- 250 MHz

  -- DUT signals
  signal clk          : std_logic := '0';
  signal rst          : std_logic := '1';

  signal mac_tdata    : std_logic_vector(63 downto 0) := (others => '0');
  signal mac_tkeep    : std_logic_vector(7 downto 0)  := (others => '0');
  signal mac_tvalid   : std_logic := '0';
  signal mac_tlast    : std_logic := '0';
  signal mac_tready   : std_logic;

  signal m_tdata      : std_logic_vector(63 downto 0);
  signal m_tkeep      : std_logic_vector(7 downto 0);
  signal m_tvalid     : std_logic;
  signal m_tlast      : std_logic;
  signal m_tready     : std_logic := '1';   -- always ready to drain TX

  signal s_wr_en      : std_logic := '0';
  signal s_wr_addr    : std_logic_vector(3 downto 0)  := (others => '0');
  signal s_wr_data    : std_logic_vector(31 downto 0) := (others => '0');
  signal s_commit     : std_logic := '0';

  signal now_ns       : std_logic_vector(63 downto 0) := (others => '0'); -- 0 => internal time
  signal session_id   : std_logic_vector(31 downto 0) := x"DEADBEEF";
  signal cfg_udp_port : std_logic_vector(15 downto 0) := x"1F90";         -- UDP 8080

  signal q_cpu_reg    : std_logic_vector(31 downto 0);
  signal load_err     : std_logic_vector(1 downto 0);

  -- TX monitor (captured from beat 2 of the latest frame)
  signal got_frame    : std_logic := '0';
  signal bid_cap      : tick_t := (others => '0');
  signal ask_cap      : tick_t := (others => '0');

  signal sim_done     : boolean := false;

  -- byte / frame helper types
  type byte_t  is array(natural range <>) of std_logic_vector(7 downto 0);

begin

  -- -----------------------------------------------------------------------
  -- Clock
  -- -----------------------------------------------------------------------
  clk_gen : process
  begin
    while not sim_done loop
      clk <= '0'; wait for TCLK/2;
      clk <= '1'; wait for TCLK/2;
    end loop;
    wait;
  end process;

  -- -----------------------------------------------------------------------
  -- DUT
  -- -----------------------------------------------------------------------
  dut : entity work.mmx_dataplane
    port map(
      clk          => clk,          rst        => rst,
      mac_tdata    => mac_tdata,    mac_tkeep  => mac_tkeep,
      mac_tvalid   => mac_tvalid,   mac_tlast  => mac_tlast,
      mac_tready   => mac_tready,
      m_tdata      => m_tdata,      m_tkeep    => m_tkeep,
      m_tvalid     => m_tvalid,     m_tlast    => m_tlast,
      m_tready     => m_tready,
      s_wr_en      => s_wr_en,      s_wr_addr  => s_wr_addr,
      s_wr_data    => s_wr_data,    s_commit   => s_commit,
      now_ns       => now_ns,       session_id => session_id,
      cfg_udp_port => cfg_udp_port,
      q_cpu_reg    => q_cpu_reg,    load_err   => load_err );

  -- -----------------------------------------------------------------------
  -- TX AXIS monitor: track beat index, capture bid:ask from beat 2.
  -- -----------------------------------------------------------------------
  tx_mon : process(clk)
    variable beat_no : integer range 0 to 7 := 0;
  begin
    if rising_edge(clk) then
      if rst = '1' then
        beat_no   := 0;
        got_frame <= '0';
      elsif m_tvalid = '1' and m_tready = '1' then
        got_frame <= '1';
        if beat_no = 2 then
          bid_cap <= signed(m_tdata(63 downto 32));
          ask_cap <= signed(m_tdata(31 downto  0));
        end if;
        if m_tlast = '1' then
          beat_no := 0;
        else
          beat_no := beat_no + 1;
        end if;
      end if;
    end if;
  end process;

  -- -----------------------------------------------------------------------
  -- Stimulus + checks
  -- -----------------------------------------------------------------------
  stim : process

    -- one synchronous ParamBlock word write (held for a single clock).
    procedure wr(a : integer; d : std_logic_vector(31 downto 0)) is
    begin
      s_wr_en   <= '1';
      s_wr_addr <= std_logic_vector(to_unsigned(a, 4));
      s_wr_data <= d;
      wait until rising_edge(clk);
    end procedure;

    -- send one ITCH-lite message wrapped in an Ethernet/IPv4/UDP frame.
    -- header bytes the RX cares about: ethertype @12..13 = 0x0800,
    -- IP proto @23 = 0x11, UDP dst port @36..37 = cfg_udp_port. Payload @42.
    procedure send_itch(mtype  : std_logic_vector(7 downto 0);
                        side_b : std_logic_vector(7 downto 0);
                        tkr    : integer;
                        price  : integer;
                        sz     : integer) is
      variable frame : byte_t(0 to 61) := (others => (others => '0'));
      variable pl    : byte_t(0 to 19);
      variable pr    : std_logic_vector(31 downto 0) := std_logic_vector(to_signed(price, 32));
      variable szv   : std_logic_vector(31 downto 0) := std_logic_vector(to_unsigned(sz, 32));
      variable tk    : std_logic_vector(15 downto 0) := std_logic_vector(to_unsigned(tkr, 16));
      variable data  : std_logic_vector(63 downto 0);
      variable keep  : std_logic_vector(7 downto 0);
      variable idx   : integer;
    begin
      -- ---- build the 20-byte ITCH-lite payload (big-endian fields) --------
      pl := (others => (others => '0'));
      pl(0)  := mtype;
      -- pl(1..8) ts_ns = 0
      pl(9)  := tk(15 downto 8);  pl(10) := tk(7 downto 0);
      pl(11) := side_b;
      pl(12) := pr(31 downto 24); pl(13) := pr(23 downto 16);
      pl(14) := pr(15 downto 8);  pl(15) := pr(7 downto 0);
      pl(16) := szv(31 downto 24);pl(17) := szv(23 downto 16);
      pl(18) := szv(15 downto 8); pl(19) := szv(7 downto 0);

      -- ---- header bytes the parser checks --------------------------------
      frame(12) := x"08"; frame(13) := x"00";          -- ethertype IPv4
      frame(14) := x"45";                              -- IP ver/ihl (cosmetic)
      frame(23) := x"11";                              -- IP protocol = UDP
      frame(36) := cfg_udp_port(15 downto 8);          -- UDP dst port hi
      frame(37) := cfg_udp_port(7 downto 0);           -- UDP dst port lo
      for i in 0 to 19 loop
        frame(42 + i) := pl(i);
      end loop;

      -- ---- stream 8 beats (62 bytes; last beat carries 6 valid lanes) -----
      for k in 0 to 7 loop
        data := (others => '0');
        keep := (others => '0');
        for i in 0 to 7 loop
          idx := k*8 + i;
          if idx <= 61 then
            data(i*8+7 downto i*8) := frame(idx);
            keep(i) := '1';
          end if;
        end loop;
        mac_tdata  <= data;
        mac_tkeep  <= keep;
        mac_tvalid <= '1';
        if k = 7 then mac_tlast <= '1'; else mac_tlast <= '0'; end if;
        -- AXIS handshake: advance only when the sink accepts the beat.
        loop
          wait until rising_edge(clk);
          exit when mac_tready = '1';
        end loop;
      end loop;
      mac_tvalid <= '0';
      mac_tlast  <= '0';
      mac_tkeep  <= (others => '0');
    end procedure;

    variable waited : integer := 0;
  begin
    -- ---- reset ----------------------------------------------------------
    rst <= '1';
    for i in 0 to 7 loop wait until rising_edge(clk); end loop;
    rst <= '0';
    wait until rising_edge(clk);

    -- ---- write + commit a valid ParamBlock ------------------------------
    --   w0 inv_skew_q31 = 0
    --   w1 half_spread  = 5
    --   w2 max_inv      = 100
    --   w3 base_qty     = 10
    --   w4 regime/kill/bid_skew : kill byte (15:8)=0 -> NOT kill
    --   w5 ask_skew(15:0)=0, requote_ticks(31:16)=1
    --   w6/w7 epoch_ns = 0 ; w8..w11 reserved 0 ; w12..w15 hmac tag (stub)
    wr(0,  x"00000000");
    wr(1,  x"00000005");
    wr(2,  x"00000064");
    wr(3,  x"0000000A");
    wr(4,  x"00000000");
    wr(5,  x"00010000");
    wr(6,  x"00000000");
    wr(7,  x"00000000");
    wr(8,  x"00000000");
    wr(9,  x"00000000");
    wr(10, x"00000000");
    wr(11, x"00000000");
    -- w12..w15 = real HMAC-SHA256 tag from host sign_param_block (key 0xab*32)
    wr(12, x"b55305c9");
    wr(13, x"34530487");
    wr(14, x"88fbee6a");
    wr(15, x"2d969112");
    s_wr_en <= '0';

    -- commit pulse
    s_commit <= '1';
    wait until rising_edge(clk);
    s_commit <= '0';

    -- wait for the real HMAC-SHA256 (4 SHA blocks ~= 280 cycles) + load/swap
    for i in 0 to 319 loop wait until rising_edge(clk); end loop;

    assert load_err = "00"
      report "ParamBlock load reported error load_err=" &
             integer'image(to_integer(unsigned(load_err)))
      severity warning;

    -- ---- drive ITCH-lite messages to move the book ----------------------
    send_itch(MSG_ADD,  x"00", 1, 100, 50);   -- ADD BID  100 x50
    for i in 0 to 3 loop wait until rising_edge(clk); end loop;
    send_itch(MSG_ADD,  x"01", 1, 110, 50);   -- ADD ASK  110 x50  -> mid 105
    for i in 0 to 3 loop wait until rising_edge(clk); end loop;
    send_itch(MSG_EXEC, x"00", 1, 100, 10);   -- EXEC BID 100 x10  -> fill, q += 10
    for i in 0 to 3 loop wait until rising_edge(clk); end loop;

    -- ---- wait up to N cycles for a TX frame -----------------------------
    waited := 0;
    while (got_frame = '0') and (waited < 1000) loop
      wait until rising_edge(clk);
      waited := waited + 1;
    end loop;

    -- give the captured beat-2 a moment to settle
    for i in 0 to 9 loop wait until rising_edge(clk); end loop;

    -- ---- checks ---------------------------------------------------------
    assert got_frame = '1'
      report "FAIL: order_encoder produced no TX frame within N cycles"
      severity failure;

    assert bid_cap < ask_cap
      report "FAIL: quote bid (" & integer'image(to_integer(bid_cap)) &
             ") not < ask (" & integer'image(to_integer(ask_cap)) & ")"
      severity failure;

    report "PASS: TX frame produced; bid=" & integer'image(to_integer(bid_cap)) &
           " ask=" & integer'image(to_integer(ask_cap)) &
           " q_cpu_reg=" & integer'image(to_integer(signed(q_cpu_reg)))
      severity note;

    sim_done <= true;
    wait;
  end process;

end architecture sim;
