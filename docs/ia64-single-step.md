# IA-64 PSR.ss single-step: retirement and next-slot restart

Tracked by [issue #45](https://github.com/ChickenParts/qemu-ia64/issues/45).
Parent: stacked [PR #44](https://github.com/ChickenParts/qemu-ia64/pull/44).
Intel Itanium SDM rev 2.3, Volume 2 §5.5.3 and §8, Table 8-3.

**Scope of this implementation (not yet guest-qualified):**

- PSR.ss is bit 40 and must be entered via CR.IPSR+RFI: neither
  mov psr.l (low 32 bits) nor ssm/rsm (24-bit immediate) can set it.
- A successful nonbranch instruction takes vector 0x6000 **after**
  its effects. Source CR.IIPA/ISR.EI identify the retired instruction;
  next CR.IIP/IPSR.RI identify the instruction to be executed on RFI.
- After source slot0 -> next RI1; slot1 -> RI2; slot2 -> next bundle/RI0.
  MLX L+X is one long instruction: L cannot independently single-step,
  and X completion reports source ISR.EI=2 and next bundle/RI0.
  Restart of the L+X pair itself uses RI1, not ISR.EI1.
- A taken B/F/X branch saves its target as next IP (RI0); SS-only
  selects 0x6000 and simultaneous TB|SS selects 0x5f00 with code 0xc.
- B4 privilege-demoting returns still select 0x5e00 ahead of TB/SS,
  with the concurrent bitvector stored in ISR.code.
- RFI never raises a step trap. A faulting helper cannot complete and
  therefore cannot take an additional post-success SS trap.
- One shared interrupt-window path preserves CR.IFS validity, saved
  PSR and RSE context. Reserved/illegal instructions must fault first.

**Qualification required before marking review-ready:** independently
assembled firmware-free IVT tests for each source slot and MLX; real
0x6000 and concurrent 0x5f00/0x5e00 code/EI/IIPA/IIP/IPSR probes; fault
suppression, false-qp NOP semantics, next-instruction RFI resume once;
full clean QEMU build and inherited stacked tests. Host C policy and
source-shape guards are necessary, not a substitute for real execution.
Keep #21 and #45 open and **ISA COMPLETE=NO**.

## Additional guest coverage

The SS validation runner now includes F14 `fchkf.s1` taken/non-taken
paths, MLX long branches `brl.cond` / `brl.call` with SS-only
and TB+SS simultaneous code, plus a BREAK fault taken before instruction
success to prove 0x6000 suppression. The long-call cases assert `b0`
return-pointer commit before trap and after RFI. These are real GNU
IA-64 guest executions; assembly is not evidence of runtime conformance.
