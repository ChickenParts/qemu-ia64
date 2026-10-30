# Merced sequential slot-2 unimplemented instruction address (UIA)

Directly stacked on PR #51 with no unrelated ISA/firmware changes.

## Architecture and explicit choice

Intel Itanium SDM rev2.3 Volume 2 §5.3, Table 5-6 footnote,
and 0x5e00 vector pp.2:205–206: a successful **nonbranching slot-2
instruction** in the last implemented bundle may produce the UIA
post-result **TRAP** instead of the alternative subsequent-fetch
**FAULT**. UIA outranks simultaneous SS. The same 0x5e00 vector serves
UIA/LP, but sequential fallthrough is not a taken branch or return.

CR.IIPA and ISR.ei name the source slot2; CR.IIP and IPSR.ri=0 name
the unimplemented next bundle. Only simultaneous enabled SS contributes
ISR.code=8; PSR.tb/lp do not add codes for a nonbranch, even if enabled.
NI follows PSR.ic; the source instruction's register changes committed.

## Implementation

After successful slot2 retirement (excluding rfi and taken branches),
the translator tests a conservative *source-PC-only* Merced PA/VA edge
candidate. It ends the TCG block before decoding the unimplemented
next bundle. The existing runtime PSR.ss gate remains at all ordinary
non-edge addresses. At an edge, the shared sequential helper executes
even with SS disabled; it selects UIA ahead of SS using the actual
PSR.it and normalized physical address. This is specific to CPU family7
(51 within-region VA bits + 3 VRN; 44 PA bits).

## Independent qualification

- 130 independent address/ISR host oracle cases across eight VRNs,
  physical edge, simultaneous SS/TB/LP/IC, near-edge controls.
- Eleven GNU IA-64 real-IVT guest profiles, with a genuine 4-KiB
  ITC.I mapping placing a source bundle at the high virtual address
  0x0003fffffffffff0. A 16-MiB identity ITC.I entry makes the IVT
  accessible under PSR.it=1. Tests include I/B/F/MLX slots,
  predicated-off branch, TB/SS/IC combinations and near-edge controls.
- Real vector, CR.ISR, CR.IIPA/IIP/IPSR/RI, committed register r10,
  and one repaired RFI are checked. Exact PR merge full QEMU build,
  inherited regressions and retained hashes/ELFs/logs required.

## Explicit limits

Physical mode at the 44-bit PA edge is host-qualified only: the real
guest tests exercise a **virtual** boundary, not 16TiB physical RAM.
Real translated fetch-fault alternative, all VRNs in real guests,
FP-result trap coincidence, data UIA, nested RSE, IA-32 transitions,
firmware and full ISA completeness remain open. #21/#45/#27 remain
open, **ISA COMPLETE=NO**, and stacked PR ancestry is preserved.
