# IA-64 B8: clrrrb and clrrrb.pr

References: [Intel Itanium Architecture Software Developer's Manual,
Volume 3 revision 2.3](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-vol-3-manual.pdf),
**instruction reference p. 3:37; encoding B8, §4.5.3.1, p. 3:355,
table 4-48, p. 3:350**.

## Defined behavior

| Form | Major | x6 [32:27] | CFM bits affected |
| --- | ---: | ---: | --- |
| `clrrrb` | 0 | 0x04 | `rrb.gr[24:18]`, `rrb.fr[31:25]`, `rrb.pr[37:32]` |
| `clrrrb.pr` | 0 | 0x05 | only `rrb.pr[37:32]` |

Both are **unpredicated** with all non-extension B8 instruction bits
fixed zero. Both **must be the last instruction in an instruction group**.
Intel rev 2.3 labels executing them without a following stop **undefined
behavior**, not a mandatory Illegal Operation fault. The positive GNU
fixtures therefore terminate their group with `;;`. Neither form raises
an architectural interruption on valid use.

All CFM fields unrelated to the specified RRBs, notably
SOF/SOL/SOR, stay unchanged. No register data or NaT bit is destroyed:
changing an RRB changes **which physical register a logical register
names**, not its physical storage.

## QEMU implementation detail: GR rematerialization

This QEMU target implements FR/PR rename with CFM-aware accessors,
but implements GR rotation by physically shifting `env->r[32:]` and
`env->nat[32:]` in `HELPER(rotate_grs)` while directly indexing
stacked GRs in the translator. This representation must be undone when
the architectural GR rename base is reset, or the visible GR window
would silently remain rotated even when `CFM.rrb.gr == 0`.

`HELPER(clrrrb)` therefore:

1. In the predicate-only form, clears **only** CFM.rrb.pr.
2. In the all-form, reorders both the physicalized GR values **and their
   NaT companions** back to the RRB=0 logical view using a temporary
   array (`new[i] = old[(i - rrbg) mod sor]`).
3. Clears the three RRB fields without touching frame sizes or
   existing floating-point and predicate *physical* storage.

The GR normalization is limited to the current SOR/SOF rotating window.
Future replacement of physicalized GR rotation with proper rename
accessors should retire this compatibility step in both rotate and
clear, but only after equivalent guest tests pass.

## Validation requirements

- `scripts/ia64-b8-clrrrb-byte-oracle.py`: 2 forms × 3 B-slot-ending
  templates = 6 GNU assembler-derived 41-bit B8 encodings. Decode
  independent assembler bytes; do not synthesize expected machine code.
- `scripts/run-ia64-b8-clrrrb-tests.py`: 12 firmware-free real-QEMU
  executions, with 0, 1, 2, 7, 8 and 9 prior `br.wtop` rotations
  for each form. Check visible GR32/GR39, FR32, p16/p17, PSR.mfl/mfh,
  and a real NaT bit carried by a rotating GR plus an adjacent clean GR,
  and exact RRB/size fields saved by a real `br.call` into `ar.pfs`.
- `tests/ia64/nonf/test_b8_clrrrb.py`: source decoder and helper
  mutation guards; inherited reviewed non-F fallback census.
- Dedicated `ia64-b8-clrrrb-validation.yml`: clean QEMU system build;
  B8 tests, prior B6/B7 and B9 real-system tests, F12/F15 IVT, F-slot
  regression, retained source/build fingerprints. Independent F-unit
  workflows continue to run on PRs.

Reproduction:

```sh
python3 scripts/ia64-nonf-fallback-audit.py --check
python3 -m unittest discover -s tests/ia64/nonf -p 'test_*.py' -v
python3 scripts/ia64-b8-clrrrb-byte-oracle.py
python3 scripts/run-ia64-b8-clrrrb-tests.py --assemble-only
python3 scripts/run-ia64-b8-clrrrb-tests.py --qemu build/qemu-system-ia64
```

## Explicitly out of scope

B8 `epc`, `vmsw.0/1`; all-B reserved/ignored/conditionally
reserved encoding classification; B4 branch-type legality; PSR.tb
taken-branch traps (#21); RSE deferred-repair integration and full
Xen/Rooster firmware boots. Neither a successful positive assembler
oracle nor complete B8 `clrrrb` execution constitutes a claim of
whole B-unit or whole IA-64 ISA conformance.
