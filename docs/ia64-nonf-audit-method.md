# IA-64 non-F instruction audit — phase 0: translator fallback census

Reference: Intel *Itanium Architecture Software Developer's Manual*, Volume 3,
revision 2.3 (May 2010), document 323207, Chapter 4.

The F1–F16 positive-family audit in `tests/ia64/isa/` is **not** an all-ISA
coverage statement. It reports 75 F families/233 forms/2,097 independently
assembled operand-profile vectors on the current F1 stack.

This new non-F phase records every source-level generic `gen_unimpl`
invocation in the production `target/ia64/translate.c` and locks its call
site and immediate source context. It distinguishes A/I/M/B/LX sites from
the F fallback, reserved-template dispatch, impossible slot default, and
dynamic break/callgate helper. The report is generated at
`docs/generated/ia64-nonf-fallbacks.md`.

**A source fallback call site is not an instruction, an opcode family, or
necessarily a defect.** One site may accept many legal encodings and may
also be reachable by reserved encodings. Conversely, an existing decoder
branch can execute with incorrect state or exceptions without ever
reaching `gen_unimpl`. These sites are an inspection and regression
starting point only.

## Reference-driven follow-up plan

Each of A, I, M, B, and L+X must receive its own manifest of architected
instruction forms and fixed fields, analogous to the F1–F16 audit. The
frozen revision's relevant sections are 4.2, 4.3, 4.4, 4.5 and 4.7.

For every form:

1. Transcribe exact fixed/opcode extension fields with section, table and
   processor-generation provenance; keep format and mnemonic distinct.
2. Expand legal operand and predicate profiles, including register aliases,
   low/high GRs/FRs, architectural constants, unusual immediate bounds
   and permitted bundle templates/slots.
3. Distinguish valid, ignored (NOP), unconditional reserved, conditional
   reserved when qualifying PR is true, and generation/feature-specific
   encodings. **Do not** equate a blank opcode-table cell to illegal.
4. Cross-check assembled bytes with independent GNU IA-64 binutils. Test
   against the real translator, not a copied Python decoder.
5. Add targeted execution, NaT/PSR/AR/CFM/RSE/ALAT state, and real-IVT
   exception tests before recording any family as architecturally complete.
6. Lock the recorded oracle, support deliberate mutation detection, and
   prohibit automatic rebaselining in read-only PR CI.

Implementation priority is **B-unit** branch selection and PSR.tb (#21),
then **M-unit** privilege/indexed-register and load/store semantics, then
**I/A** integer NaT/parallel and bitfield semantics, then **L+X** long
branch/immediate controls. These are *proposed batches*, not completeness
claims or a replacement for boot-frontier triage.

The first census is intentionally lexical; subsequent phases must ground
individual encodings against the reference. No assembler-inferred mnemonic
list, source grep count, or generic-UNIMPL disappearance is sufficient
alone for an ISA-complete claim.
