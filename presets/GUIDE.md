# Optimizer guide examples

These original examples follow [Build an In-Place IR Optimizer, Step by Step](https://optimizer.krychlic.com/)
(read November 16, 2026). Pick `guide …` in the page's preset selector. Each file
states the topic and the intended transformation in its opening comments.

**Expected means the guide's target behavior, not a claim that the configured
compiler already implements it.** The playground runs the entire current optimizer,
so later passes can simplify an example further. It transforms IR without executing
the program; reading calls, runtime errors, and intentional infinite loops are safe
to inspect here. Do not execute the error/loop examples expecting normal completion.

## The 16 steps

| Step | Preset | What to inspect |
| --- | --- | --- |
| [1](https://optimizer.krychlic.com/#step-1) | [Baseline](07_guide_01_baseline.ir) | A minimal program that should stay unchanged; compare CLI `--no-opt`. |
| [2](https://optimizer.krychlic.com/#step-2) | [Validation](08_guide_02_validation.ir) | Slots, branch targets, and sparse textual value ids. See negative fixtures below. |
| [3](https://optimizer.krychlic.com/#step-3) | [Call targets](09_guide_03_call_targets.ir) | Forward calls, builtins, and recursive calls. |
| [4](https://optimizer.krychlic.com/#step-4) | [Use rewriting](10_guide_04_use_rewriting.ir) | Unary/binary uses, stores, call arguments, branch conditions, returns, and exit codes. |
| [5](https://optimizer.krychlic.com/#step-5) | [Constant folding](11_guide_05_constant_folding.ir) | Dependent arithmetic, conversions, concatenation, and comparisons. |
| [6](https://optimizer.krychlic.com/#step-6) | [Algebraic identities](12_guide_06_algebraic_identities.ir) | Unknown inputs, neutral elements, self-comparison, and preserved reading side effects. |
| [7](https://optimizer.krychlic.com/#step-7) | [Branch folding](13_guide_07_branch_folding.ir) | True, false, and equal-target branches. |
| [8](https://optimizer.krychlic.com/#step-8) | [Branch on NOT](14_guide_08_branch_on_not.ir) | Chained negations and a shared NOT that another instruction still uses. |
| [9](https://optimizer.krychlic.com/#step-9) | [Dead code](15_guide_09_dead_code.ir) | An unused dependency chain and load disappear; unused reading calls survive. |
| [10](https://optimizer.krychlic.com/#step-10) | [Store/load forwarding](16_guide_10_store_load_forwarding.ir) | Constant forwarding, repeated loads, overwrites, and calls between loads. |
| [11](https://optimizer.krychlic.com/#step-11) | [Dead stores](17_guide_11_dead_stores.ir) | Unread scratch stores versus a slot read in the next block. |
| [12](https://optimizer.krychlic.com/#step-12) | [Unreachable blocks](18_guide_12_unreachable_blocks.ir) | A nonzero entrypoint and remapping of both branch targets and jumps. |
| [13](https://optimizer.krychlic.com/#step-13) | [Jump threading](19_guide_13_jump_threading.ir) | Empty chains on both branch edges; a nonempty block must remain. |
| [14](https://optimizer.krychlic.com/#step-14) | [Unused functions](20_guide_14_unused_functions.ir) | An unreachable mutual-recursion component, live recursion, and call/entrypoint remapping. |
| [15](https://optimizer.krychlic.com/#step-15) | [Allocation workload](21_guide_15_allocation_workload.ir) | Allocating folds, zero/multiple string uses, slot forwarding, and dead-block cleanup. |
| [16](https://optimizer.krychlic.com/#step-16) | [Full pipeline](22_guide_16_full_pipeline.ir) | Folding enables forwarding, dead stores, branches, threading, and reachability; an input-dependent loop survives. |

## Primers, traps, and outlook

| Topic | Preset |
| --- | --- |
| Integer overflow guards | [Integer guards](23_guide_integer_guards.ir) |
| Signed zero, NaN-sensitive identities, infinity, and floating division by zero | [Floating-point guards](24_guide_floating_point_guards.ir) |
| Ownership of folded/forwarded strings, multiple uses, unused allocations, escapes | [String ownership](25_guide_string_ownership.ir) |
| Self-loops and multi-block jump cycles | [Jump cycles](26_guide_jump_cycles.ir) |
| Local forwarding and loop boundaries | [Cross-block limits](27_guide_cross_block_limits.ir) |
| Sparse ids after dead-code elimination | [Sparse value ids](28_guide_sparse_value_ids.ir) |
| Future cross-block propagation, block coalescing, conversion/comparison peepholes, duplicate stores, and parameter-slot hints | [Future optimizations](29_guide_future_optimizations.ir) |
| Integer division by zero | [Division by zero](30_guide_integer_division_by_zero.ir) |
| INT32_MIN divided by -1 | [Division overflow](31_guide_integer_division_overflow.ir) |

The baseline also covers the no-work fast path. The full-pipeline fixture is useful
for timing comparisons, but it is deliberately small: it is not a performance
benchmark or a replacement for the guide's full compiler/interpreter e2e fixture.
Module organization, ownership rules, and allocation contracts describe how to
implement and test passes rather than additional IR transformations.

## Checks and format limits

Build with `make opt`, then run from the project root:

```text
python tests/guide_presets.py
python tests/guide_presets.py --optimize
```

The default check parses all 25 presets, verifies stable parser/printer round-trips,
and checks rejection of three invalid fixtures. `--optimize` additionally runs the
current optimizer and reparses every successful output. These are structural smoke
checks, not assertions that the guide's intended transformations are implemented
or that runtime output is equivalent. A timeout catches nonconverging passes.
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

### Observed with the configured compiler

At creation time, all presets parse. Most missing passes simply leave their bait
unchanged, including DCE, forwarding, dead stores, block cleanup, and unused functions.
Integer overflow arithmetic currently folds to wrapped constants, unlike the
guide's conservative rule. The `INT32_MIN / -1` example crashes the optimizer
process on this Windows build; `--optimize` reports it as a failure. These examples
remain as independent reproductions, without modifying the external compiler repo.
