# fpga/hls/scripts/run_hls.tcl
#
# Vitis HLS build script for the GEL order-router kernel.
#
# Usage (inside the AWS Vitis container):
#     cd $REPO/fpga/hls
#     vitis_hls -f scripts/run_hls.tcl
#
# Output:
#     proj_order_router/sol_vu47p/syn/{verilog,vhdl}/   ← VHDL/Verilog IP
#     proj_order_router/sol_vu47p/impl/ip/              ← packaged IP for Vivado
#
# The packaged IP is what gets pulled into the AFI build (see
# docs/AWS_F2_DEPLOY.md).

open_project proj_order_router
set_top order_router_kernel

# Source — the synthesizable kernel header.
add_files include/order_router_hls.hpp \
    -cflags "-std=c++14 -I[file normalize ../include]"

# Native testbench (bit-exact parity check).
add_files -tb test/order_router_tb.cpp \
    -cflags "-std=c++14 -I[file normalize ../include]"

open_solution sol_vu47p

# AWS F2 target: Xilinx Virtex UltraScale+ HBM VU47P, speedgrade -2.
set_part {xcvu47p-fsvh2892-2-e}

# 250 MHz reference. Real F2 user clock can run higher (300-400 MHz)
# once the pipeline closes timing, but 250 is the safe starting point.
create_clock -period 4 -name default

# Run the flow.
csim_design
csynth_design
cosim_design -trace_level all
export_design -format ip_catalog -display_name "GEL Order Router" \
    -description "GEL bit-exact OUCH-style order encoder kernel" \
    -vendor "gel" -version "1.0"

quit
