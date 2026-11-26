-- MMX — fpga/rtl/tb_ouch_encoder.vhd
-- Self-checking GHDL testbench for ouch_encoder.
--   * Drives an OP_PLACE action, captures the 3 AXIS beats (with a back-pressure
--     stall injected mid-frame) and checks the OUCH 'Enter Order' byte layout:
--     type 'O', token, side 'B', shares(qty), price(bid), TIF, capacity.
--   * Drives an OP_CANCEL action, captures the frame and checks type 'X',
--     side 0x00, shares(qty), price==0.
-- Prints "PASS" only if every field matches and m_tlast lands on the final beat.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity tb_ouch_encoder is end entity;

architecture sim of tb_ouch_encoder is
  signal clk : std_logic := '0';
  signal rst : std_logic := '1';

  signal action       : order_action_t := (op => OP_NONE, bid => (others=>'0'),
                                            ask => (others=>'0'), qty => (others=>'0'),
                                            ref => (others=>'0'));
  signal action_valid : std_logic := '0';
  signal token        : std_logic_vector(31 downto 0) := (others => '0');

  signal m_tdata  : std_logic_vector(63 downto 0);
  signal m_tkeep  : std_logic_vector(7 downto 0);
  signal m_tvalid : std_logic;
  signal m_tlast  : std_logic;
  signal m_tready : std_logic := '0';

  constant TPD : time := 4 ns;   -- 250 MHz
  signal done  : boolean := false;
  signal errors: integer := 0;

  type frame_t is array(0 to 2) of std_logic_vector(63 downto 0);
begin

  clk <= not clk after TPD/2 when not done else '0';

  dut : entity work.ouch_encoder
    port map( clk => clk, rst => rst, action => action, action_valid => action_valid,
              token => token, m_tdata => m_tdata, m_tkeep => m_tkeep,
              m_tvalid => m_tvalid, m_tlast => m_tlast, m_tready => m_tready );

  stim : process
    variable frame : frame_t;
    variable msg   : std_logic_vector(191 downto 0);

    procedure chk(cond : boolean; msg_s : string) is
    begin
      if not cond then
        report "CHECK FAILED: " & msg_s severity error;
        errors <= errors + 1;
      end if;
    end procedure;

    -- Capture one full 3-beat frame. When 'withstall', hold m_tready low after
    -- beat 0 is first presented and verify the DUT freezes tvalid/tdata (proves
    -- back-pressure is honoured), then release and consume all three beats.
    procedure grab_frame(withstall : boolean) is
      variable hold0 : std_logic_vector(63 downto 0);
      variable idx   : integer;
    begin
      -- Keep ready low; wait until beat 0 is presented (but not yet consumed).
      m_tready <= '0';
      loop
        wait until rising_edge(clk);
        exit when m_tvalid = '1';
      end loop;
      chk(m_tkeep = x"FF", "tkeep beat0");
      chk(m_tlast = '0', "tlast not on beat0");

      if withstall then
        hold0 := m_tdata;              -- ready is low: beat 0 must be held
        for i in 0 to 2 loop
          wait until rising_edge(clk);
          chk(m_tvalid = '1', "tvalid held during stall");
          chk(m_tdata = hold0, "tdata stable during stall");
        end loop;
      end if;

      -- Release: consume 3 beats on successive handshakes (ready stays high).
      m_tready <= '1';
      idx := 0;
      while idx < 3 loop
        wait until rising_edge(clk);
        if m_tvalid = '1' then         -- ready is high => handshake this edge
          frame(idx) := m_tdata;
          if idx = 2 then
            chk(m_tlast = '1', "tlast on final beat2");
          else
            chk(m_tlast = '0', "tlast not on inner beat");
          end if;
          idx := idx + 1;
        end if;
      end loop;
      m_tready <= '0';

      msg(191 downto 128) := frame(0);
      msg(127 downto 64)  := frame(1);
      msg(63 downto 0)    := frame(2);
    end procedure;

  begin
    -- reset
    rst <= '1'; action_valid <= '0'; m_tready <= '0';
    wait until rising_edge(clk);
    wait until rising_edge(clk);
    rst <= '0';
    wait until rising_edge(clk);

    -----------------------------------------------------------------------------
    -- Test 1: OP_PLACE -> Enter Order 'O'
    -----------------------------------------------------------------------------
    token        <= x"DEADBEEF";
    action       <= (op => OP_PLACE, bid => to_signed(1234567, 32),
                     ask => to_signed(1234600, 32), qty => to_unsigned(500, 32),
                     ref => x"00000001");
    action_valid <= '1';
    wait until rising_edge(clk);
    action_valid <= '0';
    action       <= (op => OP_NONE, bid => (others=>'0'), ask => (others=>'0'),
                     qty => (others=>'0'), ref => (others=>'0'));

    grab_frame(true);
    chk(msg(191 downto 184) = x"4F", "PLACE type 'O'");
    chk(msg(183 downto 152) = x"DEADBEEF", "PLACE token");
    chk(msg(151 downto 144) = x"42", "PLACE side 'B'");
    chk(msg(143 downto 112) = std_logic_vector(to_unsigned(500, 32)), "PLACE shares/qty");
    chk(msg(111 downto 80)  = std_logic_vector(to_signed(1234567, 32)), "PLACE price/bid");
    chk(msg(79 downto 72)   = x"00", "PLACE TIF=Day");
    chk(msg(71 downto 64)   = x"50", "PLACE capacity 'P'");
    chk(msg(63 downto 0)    = x"0000000000000000", "PLACE reserved zero");

    -- gap
    wait until rising_edge(clk);
    wait until rising_edge(clk);

    -----------------------------------------------------------------------------
    -- Test 2: OP_CANCEL -> Cancel Order 'X'
    -----------------------------------------------------------------------------
    token        <= x"01020304";
    action       <= (op => OP_CANCEL, bid => to_signed(999, 32),
                     ask => (others=>'0'), qty => to_unsigned(500, 32),
                     ref => x"00000001");
    action_valid <= '1';
    wait until rising_edge(clk);
    action_valid <= '0';
    action       <= (op => OP_NONE, bid => (others=>'0'), ask => (others=>'0'),
                     qty => (others=>'0'), ref => (others=>'0'));

    grab_frame(false);
    chk(msg(191 downto 184) = x"58", "CANCEL type 'X'");
    chk(msg(183 downto 152) = x"01020304", "CANCEL token");
    chk(msg(151 downto 144) = x"00", "CANCEL side 0x00");
    chk(msg(143 downto 112) = std_logic_vector(to_unsigned(500, 32)), "CANCEL shares/qty");
    chk(msg(111 downto 80)  = x"00000000", "CANCEL price zero");

    -- gap
    wait until rising_edge(clk);
    wait until rising_edge(clk);

    -----------------------------------------------------------------------------
    -- Test 3: OP_REPLACE -> Replace Order 'U' (same field mapping as PLACE)
    -----------------------------------------------------------------------------
    token        <= x"CAFEF00D";
    action       <= (op => OP_REPLACE, bid => to_signed(2222333, 32),
                     ask => to_signed(2222400, 32), qty => to_unsigned(750, 32),
                     ref => x"00000002");
    action_valid <= '1';
    wait until rising_edge(clk);
    action_valid <= '0';
    action       <= (op => OP_NONE, bid => (others=>'0'), ask => (others=>'0'),
                     qty => (others=>'0'), ref => (others=>'0'));

    grab_frame(false);
    chk(msg(191 downto 184) = x"55", "REPLACE type 'U'");
    chk(msg(183 downto 152) = x"CAFEF00D", "REPLACE token");
    chk(msg(151 downto 144) = x"42", "REPLACE side 'B'");
    chk(msg(143 downto 112) = std_logic_vector(to_unsigned(750, 32)), "REPLACE shares/qty");
    chk(msg(111 downto 80)  = std_logic_vector(to_signed(2222333, 32)), "REPLACE price/bid");
    chk(msg(79 downto 72)   = x"00", "REPLACE TIF=Day");
    chk(msg(71 downto 64)   = x"50", "REPLACE capacity 'P'");
    chk(msg(63 downto 0)    = x"0000000000000000", "REPLACE reserved zero");

    wait until rising_edge(clk);
    wait until rising_edge(clk);

    if errors = 0 then
      report "PASS" severity note;
    else
      report "FAIL: " & integer'image(errors) & " check(s) failed" severity failure;
    end if;

    done <= true;
    wait;
  end process;

end architecture sim;
