-- MMX — fpga/rtl/tb_ouch_encoder_l2.vhd
-- L2 "tester's test" for ouch_encoder: instantiates ouch_encoder_buggy (OP_PLACE
-- side byte stamped 'S' instead of 'B') and runs the same L1-style field check.
-- It reports PASS when the injected bug is DETECTED (captured side byte /= 'B'),
-- proving the OUCH side-byte assertion is load-bearing and not vacuous.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity tb_ouch_encoder_l2 is end entity;

architecture sim of tb_ouch_encoder_l2 is
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

  type frame_t is array(0 to 2) of std_logic_vector(63 downto 0);
begin

  clk <= not clk after TPD/2 when not done else '0';

  dut : entity work.ouch_encoder_buggy
    port map( clk => clk, rst => rst, action => action, action_valid => action_valid,
              token => token, m_tdata => m_tdata, m_tkeep => m_tkeep,
              m_tvalid => m_tvalid, m_tlast => m_tlast, m_tready => m_tready );

  stim : process
    variable frame : frame_t;
    variable msg   : std_logic_vector(191 downto 0);
    variable idx   : integer;
    variable mism  : integer := 0;
  begin
    -- reset
    rst <= '1'; action_valid <= '0'; m_tready <= '0';
    wait until rising_edge(clk);
    wait until rising_edge(clk);
    rst <= '0';
    wait until rising_edge(clk);

    -- Drive one OP_PLACE (spec: side byte must be 'B' = 0x42).
    token        <= x"DEADBEEF";
    action       <= (op => OP_PLACE, bid => to_signed(1234567, 32),
                     ask => to_signed(1234600, 32), qty => to_unsigned(500, 32),
                     ref => x"00000001");
    action_valid <= '1';
    wait until rising_edge(clk);
    action_valid <= '0';
    action       <= (op => OP_NONE, bid => (others=>'0'), ask => (others=>'0'),
                     qty => (others=>'0'), ref => (others=>'0'));

    -- Capture the 3-beat frame (ready held high).
    m_tready <= '1';
    idx := 0;
    while idx < 3 loop
      wait until rising_edge(clk);
      if m_tvalid = '1' then
        frame(idx) := m_tdata;
        idx := idx + 1;
      end if;
    end loop;
    m_tready <= '0';

    msg(191 downto 128) := frame(0);
    msg(127 downto 64)  := frame(1);
    msg(63 downto 0)    := frame(2);

    -- L1-style check: OP_PLACE side byte (byte 5) must be 'B' (0x42).
    if msg(151 downto 144) /= x"42" then
      mism := mism + 1;
    end if;

    wait until rising_edge(clk);

    if mism > 0 then
      report "PASS (L2 tester's test): injected bug CAUGHT -- ouch_encoder OP_PLACE "
           & "side byte was not 'B' (0x42), so the OUCH side-byte check is "
           & "load-bearing" severity note;
    else
      report "FAIL (L2): injected bug NOT caught -- the ouch_encoder side-byte check "
           & "proves nothing!" severity failure;
    end if;

    done <= true;
    wait;
  end process;

end architecture sim;
