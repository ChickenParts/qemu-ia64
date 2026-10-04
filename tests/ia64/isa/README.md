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

The current high-priority work is #8 (F1/F6/F7 decoding), #9 (bitwise and
normalization semantics), #10 (FP state/FPSR) and #11 (remaining families and
legality). No target source is modified by this audit tranche.

## Limits

The 1,864 vectors sample eight operand profiles per form; they are not an
exhaustive enumeration of operand values. Unused fields are zero. Model
availability and exhaustive ignored/reserved-bit legality are not audited.
The 21 additional unassigned opcode observations are **not** a reserved-
encoding conformance test: Intel's table color key assigns different behavior
to different blank cells. Their architectural dispositions remain pending #11.

A/I/M/B/LX are explicitly not audited yet. Numeric and architectural-state
completeness must be established with real execution tests. In particular,
PR #6's `fcvt.xf` integer examples do not establish NaTVal/fault/PSR correctness.
No firmware or private payload is needed, checked in or uploaded by this suite.
