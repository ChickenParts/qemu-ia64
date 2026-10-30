# Native Itanium rfi to a Merced unimplemented instruction address

Follow-up directly stacked after `work/ia64-uia-taken-branch-trap`
([PR #50](https://github.com/ChickenParts/qemu-ia64/pull/50)).

## Reference and model scope

Intel Itanium SDM rev2.3 Volume 2 §8, UIA / Lower-Privilege Transfer
vector 0x5e00 pp.2:205–206, and Volume 3's `rfi` description.
The architecture permits UIA as either a post-result TRAP or the later
instruction-fetch FAULT. This emulator retains #50's explicit **TRAP**
choice for native Itanium transfers, including `rfi` itself.

A native `rfi` is **not** an ordinary taken branch. Even when the restored
`CR.IPSR` sets `PSR.lp`, `PSR.tb` or `PSR.ss`, execution of `rfi`
does not itself cause those traps. UIA's `ISR.code` therefore remains
zero. `ISR.ei` and `CR.IIPA` identify the **source rfi**, whereas the
saved `CR.IIP` and **unmodified `CR.IPSR.ri`** identify the unimplemented
return target; NI is based on the post-result restored `PSR.ic`.

Eligibility uses the *restored* `PSR.it` and the existing Merced VA/PA
limits (54 total VA bits, 44 PA bits), including the emulator's
physical-mode firmware alias normalization. It does not claim IA-32
`PSR.is=1` mode switching is implemented.

## Implementation

`gen_helper_rfi` receives the source instruction's bundle PC and RI
explicitly. The helper classifies the target and executes the existing
RSE cancellation, bank switch, IFS/CFM and PSR/IP restoration. Only if
the native target is unimplemented does it push the already-restored
window and report vector 0x5e00 as a post-result trap. Valid returns
are left on the original fast path. It must never call the branch/SS
helper on an ordinary return.

## Independent qualification

`uia-rfi-policy-test.c` exercises 192 separate eligibility and source
EI/NI/code cases, including restored TB/SS/LP and RI1/RI2, physical
versus virtual width, and the explicit IA-32 exclusion.

`run-ia64-uia-rfi-tests.py` independently assembles eleven real IVT
guest profiles using GNU IA-64 binutils. Nine deliberate bad-target
cases verify the actual vector, source EI/IIPA, bad-target IIP and
restored IPSR flags/RI. Each repairs saved IIP and clears saved
TB/SS/LP/RI before one genuine `rfi`, which must resume exactly once.
Two legal-target controls must not trap. The exact PR merge must build
a complete qemu-system-ia64 and pass inherited branch/UIA/RFI/F/SS/LP
regressions before this tranche can be called qualified.

## Deliberate limits

No claim yet for UIA on **sequential slot2 at the end of implemented
VA/PA**, genuine **virtual-mode translated guest** fetches, IA-32
transitions/debugging, TLB/data UIA interaction, deep RSE nesting or
firmware boot conformance. Keep issues #21, #45, #27 open and
`ISA COMPLETE=NO`. Never merge this PR before #50 and its ancestors.
