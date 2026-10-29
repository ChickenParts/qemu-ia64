# IA-64 PSR.tb Taken Branch trap — runtime checkpoint

Normative: Intel Itanium Software Developer's Manual, Volume 2 rev 2.3,
§7.1 (p2:151), §8 (Taken Branch Trap vector 0x5f00, p2:207),
and Table 8-3 (p2:170); Volume 3 rev2.3 br/fchkf/brl.
- A completed **taken** Itanium branch with PSR.tb=1 delivers
  vector **0x5f00**, ISR.code.tb (bit2) = 1, ISR.ei = source slot,
  IIPA = source bundle, IIP = **destination bundle**, IPSR.ri=0.
  It is a *post-commit* trap, not a restartable fault.
- A nullified/untaken branch does not deliver this trap; the exception
  handler's `rfi` does not trigger a new Taken Branch trap.
- When simultaneously flagged, the lower-priority single step event
  can contribute ISR.code.ss (bit3); Intel Table 5-6 ranks TB above SS.
  When interruption collection is off, ISR.ni describes this.
  Lower Privilege Transfer traps rank **ahead** of TB and remain
  a separate implementation/priority task.
- Any correct return must resume at the **branch target**, not the
  next sequential instruction (which generic `ia64_exception(trap)`
  handles for FP post-result traps).

Implementation: `target/ia64/taken-branch.h` is a testable C policy
for target, source slot, PSR.ri and ISR.code bits.
`HELPER(taken_branch)` captures the translated return PC and
post-commit architectural state, pushes the existing interrupt-window
snapshot, saves IFS validity and targets the real 0x5f00 vector.
`gen_record_branch` is used only on taken-control-transfer paths,
after call/return side effects, and emits a runtime-gated trap helper.
Add missing loop branch records for B1 wexit/wtop and B2 cloop;
F14 and X3/X4 brl go through their existing shared record path.
No change to rfi's branch behavior, no trap for B6/B7 prediction hints.

Validation: exhaustively test the pure-host policy, guard source order,
assemble and run firmware-free real QEMU guests for B1 cond slots0/1/2,
false predicates, TB-off negative control, direct/indirect calls and
branches, B1/B2 loop side effects, F14 fchkf and X3/X4 long branches.
In the *actual* 0x5f00 IVT handler, prove ISR.ei/code, CR.IIPA=source,
CR.IIP=target, CR.IPSR.ri=0 and preserved PSR.tb; clear only the
saved tb bit before rfi, then prove target execution with committed
side effects and no unexpected repeat trap.
Build the full emulator, rerun parent B1/B2/IFS/B4/B5, B6/B7, B8/B9,
F12/F15 and F-slot suites, and retain exact merge-ref evidence.

Deferrals: priority arbitration with PSR.lp privilege demotion and
PSR.ss single-step delivery on **nonbranch** instructions, IA-32
branch debug traps, unimplemented target-address UIA priority and
full firmware/RSE coverage. This tranche must **not** close #21
or mark `ISA COMPLETE=YES` until those are independently validated.
