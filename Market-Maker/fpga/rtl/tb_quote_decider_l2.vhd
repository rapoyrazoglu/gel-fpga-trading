-- MMX — fpga/rtl/tb_quote_decider_l2.vhd
-- L2 "tester's test" for quote_decider: instantiates quote_decider_buggy (emits
-- OP_PLACE where the spec requires OP_REPLACE once an order rests) and applies
-- the L1-style op-code check. It reports PASS when the injected bug is DETECTED
-- (the second re-quote comes back as OP_PLACE instead of OP_REPLACE), proving the
-- cancel-replace op-code assertion is load-bearing and not vacuous.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity tb_quote_decider_l2 is end entity;

architecture sim of tb_quote_decider_l2 is
  signal clk : std_logic := '0';
  signal rst : std_logic := '1';

  signal quote_in     : quote_t := (bid => (others=>'0'), ask => (others=>'0'),
                                    qty => (others=>'0'), cancel_only => '0');
  signal quote_valid  : std_logic := '0';
  signal now_ns       : ts_t := (others => '0');
  signal q            : tick_t := (others => '0');
  signal pblk         : param_block_t := PARAM_BLOCK_INIT;

  signal action       : order_action_t;
  signal action_valid : std_logic;

  constant TPD : time := 4 ns;   -- 250 MHz
  signal done  : boolean := false;
begin

  clk <= not clk after TPD/2 when not done else '0';

  dut : entity work.quote_decider_buggy
    port map( clk => clk, rst => rst, quote_in => quote_in, quote_valid => quote_valid,
              now_ns => now_ns, q => q, pblk => pblk,
              action => action, action_valid => action_valid );

  stim : process
    variable op2  : order_op_t;
    variable mism : integer := 0;
    variable p    : param_block_t := PARAM_BLOCK_INIT;
  begin
    -- Params: not killed, ample inventory head-room, any tick move re-quotes.
    p.kill_switch   := '0';
    p.max_inv       := to_unsigned(100000, 32);
    p.requote_ticks := to_unsigned(0, 16);
    p.base_qty      := to_unsigned(1, 32);
    p.valid         := '1';
    pblk <= p;
    q    <= to_signed(0, 32);
    now_ns <= to_unsigned(1000, 64);

    -- reset
    rst <= '1'; quote_valid <= '0';
    wait until rising_edge(clk);
    wait until rising_edge(clk);
    rst <= '0';
    wait until rising_edge(clk);

    -- Quote 1: first quote -> OP_PLACE, arms a resting order (resting:=1).
    quote_in    <= (bid => to_signed(100, 32), ask => to_signed(110, 32),
                    qty => to_unsigned(1, 32), cancel_only => '0');
    quote_valid <= '1';
    wait until rising_edge(clk);       -- E1: sampled, resting<=1
    quote_valid <= '0';
    wait until rising_edge(clk);       -- E2: idle, resting stays 1

    -- Quote 2: order rests + price moved -> spec requires OP_REPLACE.
    quote_in    <= (bid => to_signed(200, 32), ask => to_signed(210, 32),
                    qty => to_unsigned(1, 32), cancel_only => '0');
    quote_valid <= '1';
    wait until rising_edge(clk);       -- E3: sampled with resting=1
    quote_valid <= '0';
    wait for 1 ns;                     -- let registered action settle
    op2 := action.op;

    -- L1-style check: with an order resting, the op MUST be OP_REPLACE.
    if op2 /= OP_REPLACE then
      mism := mism + 1;
    end if;

    wait until rising_edge(clk);

    if mism > 0 then
      report "PASS (L2 tester's test): injected bug CAUGHT -- quote_decider emitted "
           & "OP_PLACE where OP_REPLACE was required (order resting), so the "
           & "cancel-replace op-code check is load-bearing" severity note;
    else
      report "FAIL (L2): injected bug NOT caught -- the quote_decider op-code check "
           & "proves nothing!" severity failure;
    end if;

    done <= true;
    wait;
  end process;

end architecture sim;
