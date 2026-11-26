-- MMX — fpga/rtl/mmx_dataplane.vhd
-- Phase-B data-plane TOP. Instantiates and wires the full pipeline (spec §4):
--
--   mac_udp_rx --pl--> feed_parser --msg--> order_book --book.mid(s)--+
--                          |                                          |
--                          | (MSG_EXEC -> fill_t)                     v
--                          v                              pricing_engine_beh
--                   inventory_counter --q--+--------------------^   |
--                                          |                       |quote
--                                          +--> quote_decider <----+
--                                          |        ^
--   param_block_loader --pblk--------------+--------+   (also -> pricing_engine)
--                                                   |
--                                          quote_decider --action--> order_encoder --> TX MAC
--
-- now_ns: an external 64-bit ns time port. If left tied to 0 (no external time
-- source wired), the design substitutes a FREE-RUNNING cycle counter scaled to
-- ns: at 250 MHz one clock = CLK_PERIOD_NS (4) ns, so the counter is advanced by
-- 4 every clock. The effective time fed to param_block_loader (staleness §7.3)
-- and quote_decider (quote age §3.5) is `now_eff` = external now_ns when nonzero,
-- else the internal counter. Tie now_ns=0 in pure-FPGA builds, or drive it from a
-- PTP/host time bus to override.
--
-- VHDL-2008, single 250 MHz clock, synchronous active-high reset. Pure structural
-- top (plus the small time-counter and MSG_EXEC->fill_t demux), no latches.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity mmx_dataplane is
  port(
    clk          : in  std_logic;
    rst          : in  std_logic;
    book_clr     : in  std_logic := '0';   -- book-only clear (empties order book; inventory/params persist)

    -- ---- RX MAC AXI-Stream in (from 25G MAC) -------------------------------
    mac_tdata    : in  std_logic_vector(63 downto 0);
    mac_tkeep    : in  std_logic_vector(7 downto 0);
    mac_tvalid   : in  std_logic;
    mac_tlast    : in  std_logic;
    mac_tready   : out std_logic;

    -- ---- TX MAC AXI-Stream out (OUCH-like order frames) --------------------
    m_tdata      : out std_logic_vector(63 downto 0);
    m_tkeep      : out std_logic_vector(7 downto 0);
    m_tvalid     : out std_logic;
    m_tlast      : out std_logic;
    m_tready     : in  std_logic;

    -- ---- AXI-Lite-ish ParamBlock write port (from CPU) ---------------------
    s_wr_en      : in  std_logic;
    s_wr_addr    : in  std_logic_vector(3 downto 0);
    s_wr_data    : in  std_logic_vector(31 downto 0);
    s_commit     : in  std_logic;

    -- ---- time / config -----------------------------------------------------
    now_ns       : in  std_logic_vector(63 downto 0); -- external ns time; tie 0 to use internal counter
    session_id   : in  std_logic_vector(31 downto 0); -- OUCH session id (order_encoder)
    cfg_udp_port : in  std_logic_vector(15 downto 0); -- UDP dst port filter (mac_udp_rx)

    -- ---- inventory CPU mirror ----------------------------------------------
    q_cpu_reg    : out std_logic_vector(31 downto 0);
    dbg_qc       : out std_logic_vector(31 downto 0);  -- debug: pricing stage-1 clamped inventory (into skew multiply)
    load_err     : out std_logic_vector(1 downto 0)   -- param load status (00 ok / 01 hmac / 10 stale)
  );
end entity;

architecture rtl of mmx_dataplane is

  -- ---- mac_udp_rx -> feed_parser (UDP payload AXIS) ------------------------
  signal pl_tdata  : std_logic_vector(63 downto 0);
  signal pl_tkeep  : std_logic_vector(7 downto 0);
  signal pl_tvalid : std_logic;
  signal pl_tlast  : std_logic;
  signal pl_tready : std_logic;

  -- ---- feed_parser -> order_book / inventory demux ------------------------
  signal parse_msg   : feed_msg_t;
  signal parse_valid : std_logic;

  -- ---- order_book -> pricing_engine ---------------------------------------
  signal book_out   : book_out_t;
  signal book_valid : std_logic;

  -- ---- inventory_counter -> pricing_engine & quote_decider ----------------
  signal q_out   : tick_t;
  signal q_valid : std_logic;
  signal q_mirror: std_logic_vector(31 downto 0);

  -- ---- MSG_EXEC -> fill_t (own fills) -------------------------------------
  signal fill_sig : fill_t;

  -- ---- param_block_loader -> pricing_engine & quote_decider ---------------
  signal pblk : param_block_t;

  -- ---- pricing_engine -> quote_decider ------------------------------------
  signal quote     : quote_t;
  signal quote_vld : std_logic;

  -- ---- quote_decider -> order_encoder -------------------------------------
  signal action     : order_action_t;
  signal action_vld : std_logic;

  -- ---- time base ----------------------------------------------------------
  signal cyc_cnt : ts_t := (others => '0');  -- free-running ns counter
  signal now_ext : ts_t;
  signal now_eff : ts_t;

begin

  -- =========================================================================
  -- Time base: free-running cycle counter scaled to ns (CLK_PERIOD_NS = 4).
  -- If the external now_ns port is nonzero it overrides the internal counter.
  -- =========================================================================
  process(clk)
  begin
    if rising_edge(clk) then
      if rst = '1' then
        cyc_cnt <= (others => '0');
      else
        cyc_cnt <= cyc_cnt + to_unsigned(integer(CLK_PERIOD_NS), TS_W);
      end if;
    end if;
  end process;

  now_ext <= unsigned(now_ns);
  now_eff <= now_ext when now_ext /= to_unsigned(0, TS_W) else cyc_cnt;

  -- =========================================================================
  -- MSG_EXEC -> fill_t : our own execution reports drive the inventory counter.
  -- side/qty/price always track the current parsed message; `valid` gates so the
  -- counter only integrates on an EXEC pulse.
  -- =========================================================================
  fill_sig.valid <= '1' when (parse_valid = '1' and parse_msg.msg_type = MSG_EXEC)
                    else '0';
  fill_sig.side  <= parse_msg.side;
  fill_sig.qty   <= parse_msg.size;
  fill_sig.price <= parse_msg.price;

  q_cpu_reg <= q_mirror;

  -- =========================================================================
  -- Pipeline instances
  -- =========================================================================

  u_mac_rx : entity work.mac_udp_rx
    port map(
      clk          => clk,          rst        => rst,
      mac_tdata    => mac_tdata,    mac_tkeep  => mac_tkeep,
      mac_tvalid   => mac_tvalid,   mac_tlast  => mac_tlast,
      mac_tready   => mac_tready,
      cfg_udp_port => cfg_udp_port,
      pl_tdata     => pl_tdata,     pl_tkeep   => pl_tkeep,
      pl_tvalid    => pl_tvalid,    pl_tlast   => pl_tlast,
      pl_tready    => pl_tready );

  u_parser : entity work.feed_parser
    port map(
      clk      => clk,        rst      => rst,
      s_tdata  => pl_tdata,   s_tkeep  => pl_tkeep,
      s_tvalid => pl_tvalid,  s_tlast  => pl_tlast,
      s_tready => pl_tready,
      msg_out  => parse_msg,  msg_valid => parse_valid );

  u_book : entity work.order_book
    port map(
      clk       => clk,        rst        => rst,   clr => book_clr,
      msg_in    => parse_msg,  msg_valid  => parse_valid,
      book_out  => book_out,   book_valid => book_valid );

  u_inv : entity work.inventory_counter
    port map(
      clk       => clk,            rst       => rst,
      fill_in   => fill_sig,       max_inv   => pblk.max_inv,
      q_out     => q_out,          q_valid   => q_valid,
      q_cpu_reg => q_mirror );

  u_param : entity work.param_block_loader
    port map(
      clk       => clk,        rst       => rst,
      s_wr_en   => s_wr_en,    s_wr_addr => s_wr_addr,
      s_wr_data => s_wr_data,  s_commit  => s_commit,
      now_ns    => now_eff,
      pblk_out  => pblk,       load_err  => load_err );

  u_pricing : entity work.pricing_engine_beh
    port map(
      clk        => clk,         rst         => rst,
      s_in       => book_out.mid, q_in       => q_out,
      valid_in   => book_valid,  pblk        => pblk,
      quote_out  => quote,       quote_valid => quote_vld,
      dbg_qc     => dbg_qc );

  u_decider : entity work.quote_decider
    port map(
      clk         => clk,         rst          => rst,
      quote_in    => quote,       quote_valid  => quote_vld,
      now_ns      => now_eff,     q            => q_out,
      pblk        => pblk,
      action      => action,      action_valid => action_vld );

  u_encoder : entity work.order_encoder
    port map(
      clk          => clk,         rst          => rst,
      action       => action,      action_valid => action_vld,
      session_id   => session_id,
      m_tdata      => m_tdata,      m_tkeep      => m_tkeep,
      m_tvalid     => m_tvalid,     m_tlast      => m_tlast,
      m_tready     => m_tready );

end architecture rtl;
