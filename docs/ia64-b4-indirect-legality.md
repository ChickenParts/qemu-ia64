# IA-64 B4 indirect-branch legality

Normative reference: Intel *Itanium Architecture Software Developer's Manual*,
Volume 3 revision 2.3, B4 encoding figures and Tables 4-49/4-50; instruction
reference `br` / `br.ia`.

## Assigned cells

B4 uses major opcode 0 and the following exact `x6` / `btype` assignments:

| x6 | btype | assigned instruction |
|---:|---:|---|
| `0x20` | 0 | `br.cond` |
| `0x20` | 1 | `br.ia` |
| `0x21` | 4 | `br.ret` |

Every other B4 `btype` cell in those two tables is brown **Reserved**.  Brown
reserved words cause Illegal Operation unconditionally; predicate value does
not suppress the fault.  The `e` notation in the table is a group-ending
annotation, not another opcode assignment.

`br.ia` is itself unpredicated.  The encoding retains a nominal `qp` bitfield,
but the architecture requires it to select PR0.  Any nonzero encoded `qp`
therefore faults before ordinary predicate qualification, even when the named
predicate is false.

Legal `br.cond` and `br.ret` remain normally predicated.  Their existing branch,
return and firmware-compatibility behavior is intentionally left behind the
normal `gen_qp_skip()` path.

## Implementation boundary

`HELPER(b4_validate)` runs before B-unit predicate skipping and:

1. classifies `major`, `x6` and `btype` through the standalone production
   policy in `target/ia64/b4.h`;
2. delivers a non-access Illegal Operation for a reserved table cell;
3. delivers the same fault for `br.ia` with `qp != 0`;
4. commits no PSR, branch-register, RSE, PC or translation state.

The translated helper captures `GETPC()` directly and passes that return address
to the architectural exception constructor, preserving the source B slot in
`CR.ISR.ei` and `CR.IPSR.ri`.

## Explicitly deferred: successful `br.ia`

This tranche does **not** claim complete IA-32 instruction-set transition.
A conforming successful `br.ia` path must address, as one coherent feature:

- `PSR.di` Disabled Instruction Set Transition fault priority;
- `BSPSTORE == BSP` and required RSE / ALAT state changes;
- CFM reset and architectural PSR field transitions including `PSR.is`;
- target formation from `AR.CSD`, the selected branch register and IA-32 EIP;
- IA-32 instruction fetch, execution, interruption and return semantics;
- processor-model and firmware compatibility without treating a normal indirect
  transfer as equivalent to IA-32 mode.

Until those pieces exist, this work closes only the B4 table-legality and
unpredicated-qualification gap.  It does not relabel the current firmware
shortcut as an architectural `br.ia` implementation.

## Validation

- `tests/ia64/nonf/b4-policy-test.c`: 524,288 production-policy combinations
  over every major, x6, btype and qp.
- `scripts/ia64-b4-byte-oracle.py`: GNU-derived `br.cond`, `br.ia` and `br.ret`
  words in B slots 0/1/2, assembler rejection of qualified `br.ia`, and proofs
  that raw negative fixtures change only btype or qp.
- `scripts/run-ia64-b4-legality-tests.py`: real QEMU IVT faults for reserved
  x6/btype cells and nonzero-qp `br.ia`, including a false-predicate case that
  must still fault, plus legal conditional taken/fallthrough behavior.
- Exact source-order tests ensure validation remains before `gen_qp_skip()` and
  no successful IA-32 transition is claimed.
- The permanent workflow builds the complete emulator and reruns B8 vmsw/EPC,
  clear-RRB/NaT, B6/B7, B9 and F-unit exception regressions.

Reproduce:

```sh
cc -std=gnu2x -O2 -Wall -Wextra -Werror \
  -o /tmp/b4-policy tests/ia64/nonf/b4-policy-test.c
/tmp/b4-policy
python3 scripts/ia64-b4-byte-oracle.py
python3 scripts/run-ia64-b4-legality-tests.py --assemble-only
python3 scripts/run-ia64-b4-legality-tests.py --qemu build/qemu-system-ia64
```
