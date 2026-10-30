# IA-64 B1/B2 loops and B3/B5 calls: PSR.ss and PSR.tb priority

Directly stacked on PR #48. Reference: Intel Itanium SDM rev2.3 Volume 2
§§7.1 and 8, Table 8-3, and Volume 3 B1/B2/B3/B5 operations.

## Required post-commit behavior

- For taken branches with SS only: vector 0x6000, ISR.code=0x8.
- For taken branches with TB+SS: higher-priority vector 0x5f00,
  ISR.code=0xc. Only one interruption is delivered per source.
- Untaken branches with SS: vector 0x6000/code 0x8 even with TB on.
- CR.IIPA/ISR.EI name the actual source; CR.IIP/IPSR.ri name target
  slot0 for taken branches, or next slot/bundle for untaken branches.
- Enable bit40 from CR.IPSR+RFI, not PSR low moves or ssm/rsm. The
  handler clears SS/TB only in saved IPSR, then verifies one resume.

The 36 real GNU guest fixtures cover cloop LC2/LC0, wtop/wexit false-p6
EC2/EC1/EC0 and true-p6, ctop/cexit LC2/EC2/zero, direct B3 and
indirect B5 call target/return registers with source RI0/1/2 and
false-qualifier non-calls. Assertions inside the real IVT handler
check committed AR.LC/AR.EC, AR.PFS and branch-return pointer. A
second independent branch call after RFI snapshots actual CFM into
AR.PFS, validating one-rotation RRB.GR=7 versus no rotation, and
caller-output to callee-input CFM remapping.

All runner oracles are architecturally motivated. Assembly alone does
not prove execution: validation requires clean full qemu-system-ia64
build and every new real guest, inherited B1/B2/B5/SS/LP/TB/RFI,
source guards, exact PR merge provenance, and retained logs.

The known-gaps audit remains ISA COMPLETE=NO; issue #21 and #45
remain open. Do not merge before #48 or rewrite parent history.
