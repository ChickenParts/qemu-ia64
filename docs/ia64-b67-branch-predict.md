# IA-64 B6/B7 branch prediction hints

Production tranche stacked after the B9 `break.b` fix (#29). Reference:
Intel *Itanium Architecture Software Developer's Manual*, Volume 3
revision 2.3, document 323207, pp. 3:32–34 and 3:354–355, especially
format diagrams B6/B7 and tables 4-55 through 4-58. GNU independent
encoding reference: binutils 2.43 `opcodes/ia64-opc-b.c`.

## Architectural contract

`brp` supplies advance **microarchitectural** prediction information.
It has no architectural state effect; for a non-predicting QEMU target,
the correct positive-form execution path is an architectural no-op,
not a branch, a BR register write, or an exception. The branch target
and tag address operands do not cause any architectural memory or
register access.

Unlike B1–B5 and B9, B6 and B7 **are not predicated**. The low
bits [4:3] are the branch-prediction `wh` field, *not* `qp`. They
must be recognized before the target's normal `gen_qp_skip(qp)`.

| Format | Major | Extension | Valid positive hint variants |
|---|---:|---:|---|
| B6 IP-relative | 7 | wh[4:3] 0–3; ih[35] 0/1 | `brp.sptk`, `brp.dptk`, `brp.loop.imp`, `brp.exit.imp`, plus `.imp` variants of sptk/dptk |
| B7 indirect | 2 | x6[32:27]=0x10; wh=0/2; ih=0/1 | `brp.sptk/dptk[.imp] b2,tag13` |
| B7 return | 2 | x6[32:27]=0x11; wh=0/2; ih=0/1 | `brp.ret.sptk/dptk[.imp] b2,tag13` |

Positive-form fixed fields: B6 bits 5 and 2:0 are zero.
B7 bits 36, 26:16, 5, 3 and 2:0 are zero. Other opcode values remain
on the existing generic fault/UNIMPL route, rather than a global B-slot
NOP. This tranche does **not** claim the remaining ignored, reserved,
or conditional-reserved encoding space has been exhaustively classified.

**Important reference nuance:** B6 `.loop` or `.exit` with **no**
`.imp` has *undefined prediction-hint effect* in the Intel manual.
This is not evidence of illegal operation or of a defined execution
effect. The positive test set uses their explicit `.imp` variants.
QEMU intentionally does not model prediction performance.

## Tests

- `scripts/ia64-b67-byte-oracle.py`: 14 GNU assembler-derived 41-bit
  instruction words, including all positive B6/B7 whether/importance
  families, three legal B-slot positions, branch registers b0–b7,
  nonzero tag and IP-relative target displacements, and fixed-zero bits.
- `scripts/run-ia64-b67-predict-tests.py`: 14 real `qemu-system-ia64`
  no-architectural-effect guests using the established firmware-free
  IVT harness. These must fall through without exception and preserve
  B7 BR values and PSR.mfl/mfh.
- `tests/ia64/nonf/test_b67_prediction.py`: source-order and guarded
  fixed-field checks, so the hints cannot silently regress into a
  generic predicated path.
- The permanent read-only `ia64-b67-validation.yml` GitHub workflow
  also reruns the B9 real-IVT, F12/F15 IVT and F-slot regressions.

Reproduce:

```sh
python3 scripts/ia64-nonf-fallback-audit.py --check
python3 -m unittest discover -s tests/ia64/nonf -p 'test_*.py'
python3 scripts/ia64-b67-byte-oracle.py
python3 scripts/run-ia64-b67-predict-tests.py --assemble-only
python3 scripts/run-ia64-b67-predict-tests.py --qemu build/qemu-system-ia64
```

## Not completed

B8 `clrrrb` / `clrrrb.pr`, `epc` and processor-dependent `vmsw`;
B4/B5 detailed legality and faults; architected negative B-unit
encodings (including false-qualification cases); and target-wide
PSR.tb Taken Branch traps (#21). All remain tracked in #27/#7.
