# MMX — fpga/rtl/build_sim.tcl
# xsim simulation flow for the Phase-B data plane (mmx_dataplane + testbench).
#
# Run from this directory with either:
#     vivado -mode batch -source build_sim.tcl
#   or (any Tcl shell on the Vivado PATH):
#     xsct build_sim.tcl
#
# It compiles every .vhd in dependency order with xvhdl -2008, elaborates the
# testbench with xelab, and runs the simulation to completion with `xsim -R`
# (run-all). A non-"PASS" report / assertion failure aborts the sim and is
# visible in the transcript.
#
# Equivalent raw shell invocation (no Tcl), same order:
#   xvhdl -2008 mmx_pkg.vhd sha256_core.vhd hmac_verify.vhd mac_udp_rx.vhd feed_parser.vhd \
#               order_book.vhd inventory_counter.vhd pricing_engine_beh.vhd \
#               quote_decider.vhd order_encoder.vhd param_block_loader.vhd \
#               mmx_dataplane.vhd tb_mmx_dataplane.vhd
#   xelab -debug typical tb_mmx_dataplane -s tb_snap
#   xsim tb_snap -R

set rtl [file dirname [file normalize [info script]]]

# Dependency order: package first, then leaf modules, hmac, top, then tb.
set files {
    mmx_pkg.vhd
    sha256_core.vhd
    hmac_verify.vhd
    mac_udp_rx.vhd
    feed_parser.vhd
    order_book.vhd
    inventory_counter.vhd
    pricing_engine_beh.vhd
    quote_decider.vhd
    order_encoder.vhd
    param_block_loader.vhd
    mmx_dataplane.vhd
    tb_mmx_dataplane.vhd
}

foreach f $files {
    puts "== xvhdl -2008 $f"
    if {[catch {exec xvhdl -2008 [file join $rtl $f] >@ stdout 2>@ stderr} err]} {
        puts "ERROR compiling $f: $err"
        exit 1
    }
}

puts "== xelab tb_mmx_dataplane"
if {[catch {exec xelab -debug typical tb_mmx_dataplane -s tb_snap >@ stdout 2>@ stderr} err]} {
    puts "ERROR elaborating: $err"
    exit 1
}

puts "== xsim tb_snap -R"
if {[catch {exec xsim tb_snap -R >@ stdout 2>@ stderr} err]} {
    puts "ERROR simulating: $err"
    exit 1
}

puts "== done"

# =============================================================================
# Equivalent Vivado out-of-context SYNTHESIS check (elaboration/synth sanity for
# the top entity; does NOT run simulation). Run separately:
#
#   vivado -mode batch -source synth_check.tcl
#
# ...where synth_check.tcl contains:
#
#   read_vhdl -vhdl2008 {
#       mmx_pkg.vhd sha256_core.vhd hmac_verify.vhd mac_udp_rx.vhd feed_parser.vhd
#       order_book.vhd inventory_counter.vhd pricing_engine_beh.vhd
#       quote_decider.vhd order_encoder.vhd param_block_loader.vhd
#       mmx_dataplane.vhd
#   }
#   synth_design -top mmx_dataplane -part xcvu47p-fsvh2892-2-e -mode out_of_context
#   report_utilization
#   report_timing_summary
#
# (-mode out_of_context leaves top-level ports unbuffered — correct for an
#  internal data-plane block that is not the chip-level pinout.)
# =============================================================================
