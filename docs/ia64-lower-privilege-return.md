# Itanium B4 br.ret privilege-level restoration and PSR.lp priority

Normative: Intel Itanium SDM vol3 rev2.3, *br* (B4 `br.ret`,
pp 3:20–29), and vol2 rev2.3 §8 (Lower-Privilege Transfer Trap,
vector **0x5e00**, p2:205/206; ISR.code bit vector Table 8-3).

**Prior code defect:** `HELPER(ret_restore)` reconstructs the RSE
window, CFM and AR.EC but does not update PSR.cpl from AR.PFS.ppl.
This prevents the architectural return privilege demotion and
therefore PSR.lp trap delivery.

Implementation:
- `AR.PFS.ppl` is snapshotted before RSE frame reconstruction because
  the shadow-window unwind can update AR.PFS.
- `br.ret` sets CPL to `max(old PSR.cpl, PFS.ppl)`: a return may demote
  but must never grant privilege.
- When PSR.lp was set and CPL **increases numerically**, 0x5e00
  LP is delivered *after* the return frame is restored and the
  updated CPL committed. LP has priority over simultaneous PSR.tb.
  The ISR.code bit vector also reports concurrent TB and SS.
- The LP trap uses IIPA and ISR.ei for the source instruction, while
  IIP=branch target and IPSR.ri=0 resume at the committed destination;
  IPSR.cpl captures **new** target CPL. The existing interrupt window
  and IFS validity mechanics are preserved.
- For non-demoting returns, an enabled PSR.tb still raises the existing
  Taken Branch trap. Ordinary branches, call, B6/B7 hints and rfi are
  unchanged.

Independently test the production policy for 4 old CPL values ×
4 saved PPL values × 16 PSR flags × 4 source RI values = 1024.
Real GNU-assembled, firmware-free QEMU guests cover br.ret in all
three branch slots with LP, TB, both, neither and false-qp; saved
PPL 0/1/2/3; the 0x5e00 vs 0x5f00 vector selection; ISR.code,
CR.IIPA/source and CR.IIP/destination, CR.IPSR.ri=0, preserved
source flags and committed CPL; `rfi` restores target CPL.

Defer PSR.ss nonbranch 0x6000 tests and actual privilege-level
transfer access checks under translated virtual pages. Leave issue
#21 OPEN, `ISA COMPLETE=NO` and the PR dependency chain intact.
