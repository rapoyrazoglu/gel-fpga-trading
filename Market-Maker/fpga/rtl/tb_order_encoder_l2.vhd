-- MMX — fpga/rtl/tb_order_encoder_l2.vhd
-- L2 "tester's test" for order_encoder: instantiates order_encoder_buggy (bid/ask
-- legs swapped in beat 2) and runs the same L1-style field checks the real
-- testbench would. It reports PASS when the injected bug is DETECTED (i.e. the
-- captured beat-2 bid word does NOT equal the driven bid), proving the parity
-- check on the two legs is load-bearing and not vacuous.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity tb_order_encoder_l2 is end entity;

architecture sim of tb_order_encoder_l2 is
  signal clk : std_logic := '0';
  signal rst : std_logic := '1';

  signal action       : order_action_t := (op => OP_NONE, bid => (others=>'0'),
                                            ask => (others=>'0'), qty => (others=>'0'),
                                            ref => (others=>'0'));
  signal action_valid : std_logic := '0';
  signal session_id   : std_logic_vector(31 downto 0) := (others => '0');

  signal m_tdata  : std_logic_vector(63 downto 0);
  signal m_tkeep  : std_logic_vector(7 downto 0);
  signal m_tvalid : std_logic;
  signal m_tlast  : std_logic;
  signal m_tready : std_logic := '0';

  constant TPD : time := 4 ns;   -- 250 MHz
  signal done  : boolean := false;

  type frame_t is array(0 to 7) of std_logic_vector(63 downto 0);
begin

  clk <= not clk after TPD/2 when not done else '0';

  dut : entity work.order_encoder_buggy
    port map( clk => clk, rst => rst, action => action, action_valid => action_valid,
              session_id => session_id, m_tdata => m_tdata, m_tkeep => m_tkeep,
              m_tvalid => m_tvalid, m_tlast => m_tlast, m_tready => m_tready );

  stim : process
    variable frame    : frame_t;
    variable idx      : integer;
    variable mism     : integer := 0;
    constant BID_V    : tick_t := to_signed(1234567, 32);
    constant ASK_V    : tick_t := to_signed(7654321, 32);
    constant QTY_V    : qty_t  := to_unsigned(500, 32);
  begin
    -- reset
    rst <= '1'; action_valid <= '0'; m_tready <= '0';
    wait until rising_edge(clk);
    wait until rising_edge(clk);
    rst <= '0';
    wait until rising_edge(clk);

    -- Drive one OP_PLACE with clearly-distinct bid/ask so a swap is visible.
    session_id   <= x"0000ABCD";
    action       <= (op => OP_PLACE, bid => BID_V, ask => ASK_V, qty => QTY_V,
                     ref => x"00000001");
    action_valid <= '1';
    wait until rising_edge(clk);
    action_valid <= '0';
    action       <= (op => OP_NONE, bid => (others=>'0'), ask => (others=>'0'),
                     qty => (others=>'0'), ref => (others=>'0'));

    -- Capture all 8 beats (ready held high).
    m_tready <= '1';
    idx := 0;
    while idx < 8 loop
      wait until rising_edge(clk);
      if m_tvalid = '1' then
        frame(idx) := m_tdata;
        idx := idx + 1;
      end if;
    end loop;
    m_tready <= '0';

    -- L1-style parity check on the two legs of beat 2 (spec §4.6):
    --   beat 2 : [63:32] bid price  /  [31:0] ask price
    if frame(2)(63 downto 32) /= std_logic_vector(BID_V) then
      mism := mism + 1;
    end if;
    if frame(2)(31 downto 0) /= std_logic_vector(ASK_V) then
      mism := mism + 1;
    end if;

    wait until rising_edge(clk);

    if mism > 0 then
      report "PASS (L2 tester's test): injected bug CAUGHT -- order_encoder beat-2 "
           & "bid/ask leg check flagged " & integer'image(mism)
           & " mismatched word(s), so the leg-parity check is load-bearing"
           severity note;
    else
      report "FAIL (L2): injected bug NOT caught -- the order_encoder field check "
           & "proves nothing!" severity failure;
    end if;

    done <= true;
    wait;
  end process;

end architecture sim;
