# IA-64 ISA audit

This directory implements the first tranche of #7: a named-form F1–F16
inventory and an executable **known-gap audit**, not an ISA-completion claim.

`coverage.json5` records 75 canonical families / 233 precision, status and
conditional forms. The field facts were transcribed from Intel Volume 3,
revision 2.3 (May 2010), section 4.6. Operand packing is independently checked
against GNU IA-64 assembly, including the exact `fcvt.xf f7=f7` firmware word.
The compiler/disassembler tables were not copied into QEMU.

## Run

```sh
python3 -m pip install -r tests/ia64/isa/requirements.txt
python3 scripts/ia64-isa-coverage.py --check --output observations.json5
python3 -m unittest discover -s tests/ia64/isa -p 'test_*.py' -v
python3 scripts/ia64-isa-coverage.py --check --assembler --evidence-dir evidence
```

The independent lane needs `ia64-linux-gnu-as`, `ia64-linux-gnu-ld` and
`ia64-linux-gnu-objcopy`. Override them with `IA64_AS`, `IA64_LD` and
`IA64_OBJCOPY`; `CC` selects the host C compiler. Commands are split with
`shlex` and executed without a shell. All subprocesses are time-bounded.
The retained evidence includes the generated assembly and toolchain version.
CI installs the audit-only JSON5 dependency; QEMU gets no runtime dependency.

The host harness compiles the actual production F-slot C block, replacing
TCG emitters with recording stubs. It is **not** a duplicate handwritten
recognizer. It sees C translation decisions, not the generated guest control
flow, numerical results, predicates at execution time or exceptions. The
explicit emitter allowlist and source-shape guards fail closed when that
projection needs review.

The report is `docs/generated/ia64-instruction-coverage.md`. Its baseline
locks each vector's raw bits and emitted route, not only aggregate counts.
A passing check means the known gaps remain accurately recorded. It is not
permission to merge incorrect CPU semantics or close completion issues.

## Intentional changes

Implement a scoped family fix and add execution/state/exception tests first.
Inspect `--output` before and after the change, update the corresponding
known-gap assertions, then run `--record` to refresh the JSON5 measurement
and generated report. CI never rebaselines automatically. Retain independent
assembler verification after changing the encoding registry or format packer.

PR #13 repaired F1/F6/F7 selection. F9 now has a dedicated integer-only
implementation for all 19 families; the report still labels shared FP-state
limitations. #10 (FP state/FPSR) and #11 (remaining families and legality) stay
open. The audit command itself never modifies target source.

## F9 execution evidence

`test_f9.py` executes the actual production F9 helper and FR accessors in a
host C harness. It compares 4,864 directed pattern pairs and 19,456 seeded
random pairs against a separate bit-string oracle, plus 14,592 helper calls
covering all 96 FP-rotation values and destination/source aliases. It checks
NaTVal, illegal-destination fault requests, PSR.mfl/mfh and untouched state.
The fault hook observes requests; it does not emulate QEMU exception delivery.
A deliberately corrupted merge implementation proves the numerical tests
catch errors that a decode-only probe cannot.

```sh
python3 scripts/run-ia64-f9-tests.py --qemu build-f9/qemu-system-ia64
```

This builds a firmware-free guest with 4,256 cases across all 19 families.
Each case checks both spill words, PSR dirty bits, and unchanged FPSR;
source/destination aliases, low/high FRs, constant sources, NaTVal, SP
denormals, and true/false predication are included. Its separate data segment
is pinned into physical RAM rather than the GNU IA-64 default region-3 VMA.
The runner rejects stale logs, FAIL/UNIMPL output and missing completion,
and terminates QEMU within a bounded time.

The F9 decoder header is part of the audit source fingerprint. Neither
selection nor data-path correctness certifies all architectural faults:
disabled-FP register delivery and its exception priority remain in #10.
F9 does not use FPSR rounding/status fields; its bit operations leave FPSR
unchanged and update the destination bank's PSR modification bit.

References: Intel SDM Vol. 3 rev. 2.3, F9 and the individual instruction
operations (fand, fmerge, fmix, for, fpack, fpmerge, fswap, fsxt, fxor).
The fp_single memory-format bit wiring was cross-checked against HP's
GPL-2.0-or-later SKI `freg2sgl`, not copied as a numeric implementation:
`trofi/ski` commit `dfc2902ea1423d9b32543a5daf8026213f2b37a1`,
`src/exec.incl.c`. All new data-path code is integer-only.

## Limits

The 2,097 vectors sample nine operand profiles per form; they are not an
exhaustive enumeration of operand values. Unused fields are zero. Model
availability and exhaustive ignored/reserved-bit legality are not audited.
The 21 additional unassigned opcode observations are **not** a reserved-
encoding conformance test: Intel's table color key assigns different behavior
to different blank cells. Their architectural dispositions remain pending #11.

A/I/M/B/LX are explicitly not audited yet. Numeric and architectural-state
completeness must be established with real execution tests. In particular,
PR #6's `fcvt.xf` integer examples do not establish NaTVal/fault/PSR correctness.
No firmware or private payload is needed, checked in or uploaded by this suite.
