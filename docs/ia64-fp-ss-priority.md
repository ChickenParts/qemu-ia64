# IA-64 FP exception vs. PSR.ss priority

Intel Itanium SDM rev2.3 **Volume 2**, Table 8-3 and Floating-point Trap
vector **0x5d00** (p2:204) describe one `ISR.code` bit vector for all
simultaneous enabled traps on the retiring instruction.

## Architectural distinction

- An **inexact FP result trap (0x5d00)** is post-result: the FR result,
  FPSR sticky state and dirty bits commit. A simultaneous `PSR.ss=1`
  contributes **ISR.code bit3 (0x8)** to the higher-priority FP trap's
  code bit0 plus normal FP-specific bits13/14. A separate 0x6000 should
  **not** occur for that same instruction.
- An FP **fault (0x5c00)** is pre-result: its `ISR.code` is an FP-fault
  code, *not* the general trap bit vector. Do not OR the SS bit into an
  FP fault. `CR.IIP/IPSR.ri` restart the faulting F slot.
- With FP inexact trap disabled in FPSR, a completed F10 operation with
  PSR.ss still raises the **0x6000 Single Step** trap; no FP cause bit.
- A false qualifying predicate completes as a NOP and is independently
  checked for single-step delivery.
- All tests enable `PSR.ss` via `CR.IPSR` + `rfi`, leaving live PSR.ic
  clear during `rfi`; the handler clears SS in saved IPSR before its
  one true `rfi` resume.

## Implementation

`target/ia64/fp-trap-priority.h` is a pure host-testable policy.
`ia64_fp_interrupt()` invokes it **only** for an actual post-result FP
trap, before the common `ia64_exception` captures the state. Neither
the F10 input/fault ordering nor the general exception path is modified.

## Qualification

The separate exact-merge workflow must compile 512 policy combinations,
run fail-closed source tests and GNU-assembled guest fixtures, clean-build
`qemu-system-ia64`, execute them on real IVT vectors and carry the
inherited PSR.ss, PSR.lp, PSR.tb, F12/F15 and F10 regressions. Preserve
`ISA COMPLETE=NO` and leave issues #21/#45 open for wider FP/SS
interactions, UIA, IA-32 and firmware/RSE behavior.
