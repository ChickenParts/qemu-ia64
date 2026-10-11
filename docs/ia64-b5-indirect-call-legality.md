# IA-64 B5 indirect-call legality

Intel Itanium SDM Volume 3 revision 2.3, B5 and Table 4-53, assigns only
four 3-bit indirect-call whether hints:

| wh | completer |
|---:|---|
| 1 | `.sptk` |
| 3 | `.spnt` |
| 5 | `.dptk` |
| 7 | `.dpnt` |

The even values 0, 2, 4 and 6 are Reserved B-space encodings. They deliver
Illegal Operation before predicate qualification; a false encoded qp cannot
turn a reserved call into a no-op. Legal odd-wh calls retain normal predicate,
call-frame, return-pointer and branch behavior.

`HELPER(b5_validate)` runs before `gen_qp_skip()`, captures the translated
helper return address for exact B-slot fault metadata, and commits no PFS, CFM,
branch-register, PC, RSE or target state. The existing call implementation is
entered only after validation and predicate qualification.

Validation includes:

- an exhaustive production policy over all major/wh combinations;
- GNU-derived `.sptk/.spnt/.dptk/.dpnt` call words in B slots 0/1/2;
- reserved fixtures produced by changing only `wh` in complete GNU bundles;
- real IVT faults for all four even values, including wh=2 with p6=false;
- checks that b1, ar.pfs and the call target are untouched on fault;
- legal taken-call return-pointer and false-predicate fallthrough cases;
- inherited B4, vmsw, EPC, clear-RRB, B6/B7, B9 and F-unit regressions.

This tranche does not claim target-wide PSR.tb taken-branch traps (#21), all
reserved B-space combinations, firmware completion, or whole-ISA conformance.
The committed non-F audit remains `ISA COMPLETE=NO`.
