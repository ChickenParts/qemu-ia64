# IA-64 B1/B2 table legality

Normative: Intel *Itanium Architecture Software Developer's Manual*, Volume 3
rev. 2.3, §4.5, Table 4-47 and B1/B2 encoding diagrams.

Major opcode 4 has six assigned btype cells: B1 `br.cond` = 0,
`br.wexit` = 2, `br.wtop` = 3; B2 `br.cloop` = 5,
`br.cexit` = 6, `br.ctop` = 7. Brown **Reserved** cells 1 and 4
fault unconditionally, even when their qualifying predicate is false.
B2 bits 5:0 are **fixed zero**: nonzero qp must also fault before normal
predication. B1 keeps its ordinary predicate semantics.

Production `b12.h` classifies the table once for helper and exhaustive
host tests. `b12_validate` is called before ordinary predicate skip;
it captures GETPC in the translated helper for accurate source-slot
CR.ISR.ei and CR.IPSR.ri, delivers a **non-access Illegal Operation**,
and commits no loop/predicate/branch state. Existing legal B1/B2
execution remains unchanged in this focused tranche.

Validation: 8,192 policy cases; 7 independently GNU-assembled words covering all six assigned
forms and 4 controlled negative field mutations; 8 firmware-free
full-system IVT profiles for illegal btypes, B2 qp restrictions, legal
B1 taken/false-predicate and B2 counted taken/untaken control. Check
real IVT 0x5400, CR.IIP, CR.IIM, CR.ISR.ei, CR.IPSR.ri, unchanged
AR.LC/EC and RFI. Full QEMU build; inherited B5/B4/vmsw/EPC/clrrrb/
B6/B7/B9/F12/F15/F-slot regressions.

Not claimed: whole B1/B2 loop/epilog correctness, reserved/ignored cases
outside Table 4-47, PSR.tb taken-branch traps (#21), RSE firmware,
or ISA completion. Keep `ISA COMPLETE=NO`.

Reproduce:

```sh
cc -std=gnu2x -O2 -Wall -Wextra -Werror -o /tmp/b12-policy tests/ia64/nonf/b12-policy-test.c
/tmp/b12-policy
python3 scripts/ia64-b12-byte-oracle.py
python3 scripts/run-ia64-b12-legality-tests.py --assemble-only
python3 scripts/run-ia64-b12-legality-tests.py --qemu build/qemu-system-ia64
```
