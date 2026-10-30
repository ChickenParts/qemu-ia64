# Itanium packed F1 completion traps combined with PSR.ss

Stacked after review-ready [PR #47](https://github.com/ChickenParts/qemu-ia64/pull/47),
which implemented shared 0x5d00 FP > 0x6000 SS priority.

The new test matrix independently exercises the *other* user of the
post-result FP trap path, F1 packed \`fpma/fpms/fpnma\` (Intel Itanium
SDM rev2.3 Vol3 F1). The existing F1 guest oracles supply exact packed
binary32 results and high-/low-lane ISR completion encoding.

- \`fpma.s0\` low inexact with SS and without SS, high-only inexact with
  SS, both lanes inexact with SS: 0x5d00, with 0x8 SS bit alongside
  FP bit0, low-lane bits9/10 and/or high-lane bits13/14.
- Masked FP inexact: FPSR sticky I and committed F6, but **SS-only**
  0x6000 and ISR.code=0x8.
- Pre-result high invalid FP **fault**: vector 0x5c00, code 0x1
  (no SS); unchanged destination, FPSR and dirty state, saved RI2
  restart, handler skips the source via real RFI.
- Exact \`fpms.s1\` and \`fpnma.s2\` complete with SS-only 0x6000,
  preserving independently pinned packed results; predicated-off F1
  behaves as a NOP but SS still fires.
- RFI enters the actual F-slot (RI2) via CR.IPSR.ss bit40, leaving
  live PSR.ic clear; handlers clear SS only in saved IPSR before
  verifying one restart at the next bundle, no duplicate 0x6000.
- Every guest checks real IVT vector, ISR.code/EI, source IIPA,
  next-or-fault IIP/IPSR.ri, committed versus pre-result F6/FPSR
  and saved PSR dirty state.
- The new validation workflow also runs original seven F1
  fault/trap fixtures and inherited FP/SS, SS, LP, TB suites on
  the same clean-built emulator, with exact-PR-merge evidence.

No new production handling is assumed necessary: all F1 result traps
already call the common helper modified in PR #47. If the independent
guest oracles disagree, inspect actual logs and repair the emulator
rather than weakening or deleting tests.

Keep issues #21/#45/#10 open, **ISA COMPLETE=NO**, no forced merges.

## Exhaustive host-side ISR.code gate

A portable C test enumerates all **256** combinations of the low/high
packed completion flags at ISR.code[14:7], under the **16** combinations
of concurrent PSR.ss/tb/ic/lp, for **4,096** independently computed
policies. This supplements (and does not replace) the nine real IA-64
F1 guest executions and the inherited FP+SS policy's 512 cases.
