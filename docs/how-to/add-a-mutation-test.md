# Add a mutation test for an RTL module

A testbench that passes proves little until you have seen it fail. Every
module here has a second testbench that runs the same checks against a
deliberately broken copy and passes only if the bug is caught.

Say you have `foo.vhd` and its testbench `tb_foo.vhd`.

1. **Copy the module** to `Market-Maker/fpga/rtl/mutants/foo_buggy.vhd` and
   rename the entity to `foo_buggy`.

2. **Inject one realistic bug.** Good ones are an off-by-one in a comparison,
   a dropped term, a wrong rounding constant. Say what you changed in a comment
   at the top of the file.

3. **Copy the testbench** to `tb_foo_l2.vhd`. Instantiate
   `entity work.foo_buggy` instead of `foo`.

4. **Invert the verdict.** Where the original reports `PASS` when outputs match
   the expected values, the `_l2` bench reports `PASS` when they do **not**
   match, and fails with an assertion when they do.

5. **Run the regression.** The script picks up both files by name.

   ```sh
   tools/run_offline_regression.sh
   ```

If the `_l2` bench fails, the original testbench cannot see your bug. Strengthen
`tb_foo.vhd` first, then mirror the new check into `tb_foo_l2.vhd`.

[`tb_circuit_breaker_hw_l2.vhd`](../../Market-Maker/fpga/rtl/tb_circuit_breaker_hw_l2.vhd)
is a short example to copy from.
