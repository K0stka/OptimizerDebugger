# Optimizer debugger presets

Open a preset and read the first comments for the specific change to look for.
The reduced fractions and Simpson examples combine multiple passes on actual
compiler-lowered IR. Smaller examples make their individual patterns easier to
spot. Stack returns, physical storage reuse, and branch emission are codegen
decisions. Inspect emitted IFJcode26 for their effect.

| Topic | Preset |
| --- | --- |
| Constant recursion | [constant recursive evaluation](32_constant_recursive_evaluation.ir) |
| Specialization and clone reuse | [call specialization and reuse](33_call_specialization_and_reuse.ir) |
| Tail recursion and constant specialization | [tail recursion and specialization](34_tail_recursion_and_specialization.ir) |
| Lowered builtin effects and typed constant folding | [lowered builtin effects](35_lowered_builtin_effects.ir), [typed constant conversion](52_typed_constant_conversion.ir) |
| Streamed interpolation | [streamed interpolation](36_streamed_interpolation.ir) |
| Exact unrolling, including a guarded loop | [exact loop unrolling](37_exact_loop_unrolling.ir), [guarded loop expansion](46_guarded_loop_expansion.ir) |
| Pair unrolling and equality termination | [pair unrolling with equality exit](38_pair_unrolling_with_equality_exit.ir), [dynamic loop fast and cleanup paths](39_dynamic_loop_fast_and_cleanup_paths.ir) |
| Dynamic step dispatch | [dynamic range direction unswitching](40_dynamic_range_direction_unswitching.ir) |
| Loop slot rotation | [loop slot rotation](41_loop_slot_rotation.ir) |
| Reduced fractions: inlining, rotation, and zero-quotient bypass | [reduced fractions pipeline](42_reduced_fractions_pipeline.ir) |
| Simpson integration: parity, weight promotion, and streaming | [simpson pipeline](43_simpson_pipeline.ir) |
| Unused argument removal, preserved reads, and stack-return codegen | [unused arguments and stack returns](44_unused_arguments_and_stack_returns.ir) |
| Closed-form range sum | [constant range sum](45_constant_range_sum.ir) |
| Preserved fault order and unordered comparisons | [preserved loop fault order](47_preserved_loop_fault_order.ir), [unordered comparison guard](49_unordered_comparison_guard.ir) |
| Straight-line and CFG inlining | [inlining repeated effectful calls](48_inlining_repeated_effectful_calls.ir), [cfg inlining](51_cfg_inlining.ir) |
| Loop-invariant motion | [loop invariant motion](50_loop_invariant_motion.ir) |
| Storage reuse, direct startup, and fused branches in codegen | [codegen storage and branches](53_codegen_storage_and_branches.ir) |

## Checks and format limits

Build with `make opt`, then run from the project root:

```text
python tests/guide_presets.py
python tests/guide_presets.py --optimize
```

The default check parses all presets, verifies stable parser/printer round-trips,
and checks rejection of three invalid fixtures. With `--optimize`, the script
also reparses optimized IR and checks representative transformations for the
current examples. A timeout catches nonconverging passes.
Use `--binary <path>` to test a different build, including the debug flavor.

Negative fixtures live outside the selectable presets so normal examples load:

- [Invalid block target](../tests/fixtures/guide/invalid_block_target.ir)
- [Undefined value](../tests/fixtures/guide/invalid_value.ir)
- [Unknown call](../tests/fixtures/guide/unknown_call.ir)

The text parser rejects these before optimization. Testing malformed internal
opcodes, ids, missing terminators, or a stale internal `value_count` requires
constructing IR directly in the compiler's C unit tests. The parser maintains
those invariants and preserves sparse textual value ids.

Step 15 likewise needs the compiler test harness to inject allocation failures:
recreate the fixture for each failure index, enable the context's nth-allocation
failure setting immediately before optimization, catch the expected internal
error, then free the context under a memory checker. The preset supplies an
ownership-heavy workload; the playground CLI exposes no allocation-failure switch.
A successful normal run alone does not prove failure cleanup or absence of leaks.
