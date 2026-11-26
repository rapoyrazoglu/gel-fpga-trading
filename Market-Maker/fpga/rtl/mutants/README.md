# Mutants

Each file here is a copy of a module from the parent directory with **one bug
injected on purpose**. They are never synthesized.

The `tb_*_l2.vhd` testbenches in the parent directory instantiate these and
pass only when the bug is detected. That is how the ordinary testbenches are
shown to be able to fail.

| Mutant | Checked by |
|---|---|
| `circuit_breaker_hw_buggy` | `tb_circuit_breaker_hw_l2` |
| `hmac_verify_buggy` | `tb_hmac_verify_l2` |
| `hw_backtester_buggy` | `tb_hw_backtester_l2` |
| `obi_skew_buggy` | `tb_obi_skew_l2` |
| `order_encoder_buggy` | `tb_order_encoder_l2` |
| `ouch_encoder_buggy` | `tb_ouch_encoder_l2` |
| `param_block_loader_buggy` | `tb_param_block_loader_l2` |
| `pricing_engine_beh_buggy` | `tb_pricing_parity_l2` |
| `pricing_engine_multi_buggy` | `tb_pricing_engine_multi_l2` |
| `quote_decider_buggy` | `tb_quote_decider_l2` |
| `sha256_core_buggy` | `tb_sha256_core_l2` |
| `vol_ewma_buggy` | `tb_vol_ewma_l2` |

To add one: [docs/how-to/add-a-mutation-test.md](../../../../docs/how-to/add-a-mutation-test.md).
