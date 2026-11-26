#!/usr/bin/env bash
# Deploy the FULL data-plane CL to an AWS F2 box and wire the build to compile
# the VHDL pipeline. Run from your workstation. Idempotent. Does NOT launch the build.
#
#   F2_KEY=~/.ssh/my-key.pem F2_HOST=ubuntu@<instance-dns> ./deploy_dataplane.sh
set -euo pipefail
KEY="${F2_KEY:?set F2_KEY to the instance's .pem path}"
HOST="${F2_HOST:?set F2_HOST, e.g. ubuntu@<instance-dns>}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
RTL="$SRC/rtl"
DP="$SRC/cl_dataplane"
CL=/home/ubuntu/aws-fpga/hdk/cl/examples/cl_demo/cl_axil_reg_access

# 12 VHDL files the data plane needs, in dependency order (packages/leaves first).
VHDL_ORDERED="mmx_pkg sha256_core hmac_verify mac_udp_rx feed_parser order_book \
inventory_counter pricing_engine_beh quote_decider order_encoder param_block_loader mmx_dataplane"

echo "=== 1. backup current CL design + build scripts (once) ==="
ssh -i "$KEY" -o ConnectTimeout=25 "$HOST" "
  cd $CL
  [ -f design/cl_axil_reg_access.sv.pricing_bak ] || cp design/cl_axil_reg_access.sv design/cl_axil_reg_access.sv.pricing_bak
  [ -f design/cl_axil_reg_access_defines.vh.pricing_bak ] || cp design/cl_axil_reg_access_defines.vh design/cl_axil_reg_access_defines.vh.pricing_bak
  [ -f build/scripts/encrypt.tcl.bak ] || cp build/scripts/encrypt.tcl build/scripts/encrypt.tcl.bak
  [ -f build/scripts/synth_cl_axil_reg_access.tcl.bak ] || cp build/scripts/synth_cl_axil_reg_access.tcl build/scripts/synth_cl_axil_reg_access.tcl.bak
  echo backed up
"

echo "=== 2. copy new CL top + defines + the 12 VHDL files ==="
scp -q -i "$KEY" "$DP/cl_dataplane.sv"          "$HOST:$CL/design/cl_axil_reg_access.sv"
scp -q -i "$KEY" "$DP/cl_dataplane_defines.vh"  "$HOST:$CL/design/cl_axil_reg_access_defines.vh"
for f in $VHDL_ORDERED; do
  scp -q -i "$KEY" "$RTL/$f.vhd" "$HOST:$CL/design/$f.vhd"
done
echo "copied CL + $(echo $VHDL_ORDERED | wc -w) vhd"

echo "=== 3. patch encrypt.tcl (copy design/*.vhd into the enc dir) + synth (read_vhdl -vhdl2008) ==="
ssh -i "$KEY" -o ConnectTimeout=25 "$HOST" "python3 - <<'PY'
import re
CL='/home/ubuntu/aws-fpga/hdk/cl/examples/cl_demo/cl_axil_reg_access'

# ---- encrypt.tcl: after copying the CL .sv, copy every design/*.vhd ----
ep=CL+'/build/scripts/encrypt.tcl'; s=open(ep).read()
if 'DATAPLANE_VHDL_COPY' not in s:
    anchor='file copy -force \$CL_DIR/design/cl_axil_reg_access.sv'
    line=[l for l in s.splitlines() if anchor in l][0]
    inject=line+'\n# DATAPLANE_VHDL_COPY\nforeach f [glob -nocomplain \$CL_DIR/design/*.vhd] { file copy -force \$f \$src_post_enc_dir }'
    s=s.replace(line,inject,1); open(ep,'w').write(s); print('encrypt.tcl patched')
else: print('encrypt.tcl already patched')

# ---- synth tcl: after read_verilog, read_vhdl the 12 files in order ----
order='mmx_pkg sha256_core hmac_verify mac_udp_rx feed_parser order_book inventory_counter pricing_engine_beh quote_decider order_encoder param_block_loader mmx_dataplane'.split()
sp=CL+'/build/scripts/synth_cl_axil_reg_access.tcl'; s=open(sp).read()
if 'DATAPLANE_READ_VHDL' not in s:
    rv='\n# DATAPLANE_READ_VHDL\nread_vhdl -vhdl2008 [list \\\\\n'+' \\\\\n'.join('  \${src_post_enc_dir}/%s.vhd'%f for f in order)+' ]\n'
    m=re.search(r'read_verilog[^\n]*\n', s)
    s=s[:m.end()]+rv+s[m.end():]; open(sp,'w').write(s); print('synth tcl patched')
else: print('synth tcl already patched')
PY"

echo "=== 4. verify patches ==="
ssh -i "$KEY" -o ConnectTimeout=25 "$HOST" "
  grep -c DATAPLANE_VHDL_COPY $CL/build/scripts/encrypt.tcl
  grep -c DATAPLANE_READ_VHDL $CL/build/scripts/synth_cl_axil_reg_access.tcl
  echo 'design .vhd count:'; ls $CL/design/*.vhd | wc -l
"
echo "=== deploy complete. Next: run SynthCL (quick check), then BuildAll. ==="
