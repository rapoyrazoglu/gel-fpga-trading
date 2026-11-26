-- MMX — fpga/rtl/mmx_multi_pkg.vhd
-- Array/vector types for multi-asset parallel pricing (pricing_engine_multi).
-- Kept SEPARATE from mmx_pkg.vhd (do not edit the interface contract) so the
-- single-asset data plane is unchanged. Unconstrained arrays of the base
-- mmx_pkg element types; instances constrain them to (0 to N-1).
--
-- VHDL-2008. numeric_std only. No RTL logic here — pure type definitions.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mmx_pkg.all;

package mmx_multi_pkg is

    -- One tick_t (signed32) per lane: prices s_in and inventories q_in.
    type tick_vec_t  is array (natural range <>) of tick_t;

    -- One ParamBlock per lane (independent params per asset).
    type pb_vec_t    is array (natural range <>) of param_block_t;

    -- One quote_t per lane (parallel pricing outputs).
    type quote_vec_t is array (natural range <>) of quote_t;

end package mmx_multi_pkg;
