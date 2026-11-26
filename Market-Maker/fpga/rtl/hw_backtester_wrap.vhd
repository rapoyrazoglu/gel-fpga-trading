-- MMX — fpga/rtl/hw_backtester_wrap.vhd
-- Thin FLAT-PORT wrapper around hw_backtester (which has a `pblk : param_block_t`
-- RECORD port). Vivado's mixed-language boundary cannot map a VHDL composite
-- (record) type to a Verilog/SystemVerilog instance, so the CL top
-- (cl_axil_reg_access, SystemVerilog) cannot bind hw_backtester directly.
--
-- This wrapper exposes every port as std_logic / std_logic_vector, assembles the
-- param_block_t record from the flat AXI-Lite-driven fields, and instantiates
-- hw_backtester BY NAME. It is the exact pattern already used for mmx_dataplane
-- (all-flat ports, records built internally).
--
-- The host only drives inv_skew_q31 / half_spread / max_inv / base_qty and
-- kill_switch; the remaining ParamBlock fields are tied to sane run defaults
-- (regime_id=0, skews=0, requote_ticks=1, epoch_ns=0, valid=1) so the backtester
-- reproduces the golden testbench math compute_quote(mid,inv) = (mid-hs, mid+hs).
--
-- Pure structural, no logic, no latches. Synthesizable VHDL-2008.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

entity hw_backtester_wrap is
    generic (
        NMAX : natural := 1024
    );
    port (
        clk          : in  std_logic;
        rst          : in  std_logic;                       -- synchronous, active-high
        start        : in  std_logic;                       -- 1-cycle pulse
        n_ticks      : in  std_logic_vector(15 downto 0);

        -- flat ParamBlock fields (host-driven over AXI-Lite)
        inv_skew_q31 : in  std_logic_vector(31 downto 0);
        half_spread  : in  std_logic_vector(31 downto 0);
        max_inv      : in  std_logic_vector(31 downto 0);
        base_qty     : in  std_logic_vector(31 downto 0);
        kill_switch  : in  std_logic;

        -- external synchronous tick BRAM (1-cycle-late read)
        tick_addr    : out std_logic_vector(15 downto 0);
        tick_mid     : in  std_logic_vector(31 downto 0);

        -- results
        done         : out std_logic;
        total_pnl    : out std_logic_vector(63 downto 0);
        fills        : out std_logic_vector(31 downto 0)
    );
end entity;

architecture rtl of hw_backtester_wrap is

    signal pblk_i      : param_block_t;
    signal tick_addr_i : unsigned(15 downto 0);
    signal total_pnl_i : signed(63 downto 0);
    signal fills_i     : unsigned(31 downto 0);

begin

    -- assemble the ParamBlock record from the flat host-driven fields
    pblk_i <= (
        inv_skew_q31  => signed(inv_skew_q31),
        half_spread   => signed(half_spread),
        max_inv       => unsigned(max_inv),
        base_qty      => unsigned(base_qty),
        regime_id     => (others => '0'),
        kill_switch   => kill_switch,
        bid_skew      => (others => '0'),
        ask_skew      => (others => '0'),
        requote_ticks => to_unsigned(1, 16),
        epoch_ns      => (others => '0'),
        valid         => '1');

    tick_addr <= std_logic_vector(tick_addr_i);
    total_pnl <= std_logic_vector(total_pnl_i);
    fills     <= std_logic_vector(fills_i);

    dut : entity work.hw_backtester
        generic map (NMAX => NMAX)
        port map (
            clk       => clk,
            rst       => rst,
            start     => start,
            n_ticks   => unsigned(n_ticks),
            pblk      => pblk_i,
            tick_addr => tick_addr_i,
            tick_mid  => signed(tick_mid),
            done      => done,
            total_pnl => total_pnl_i,
            fills     => fills_i
        );

end architecture;
