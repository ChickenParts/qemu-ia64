# Merced sequential UIA across virtual regions — real-guest matrix

This is a directly stacked follow-up to PR #52 with a **real translation-order
fix**, discovered by the new guest matrix. It does not alter the UIA
trap priority or claim complete IA-64 instruction support.

## Architectural hypothesis / oracle

Original Merced advertises 51 implemented within-region virtual address
bits, plus three region bits (VRN = VA[63:61]). VA[60:51] must sign-extend
VA[50]. For **each** VRN 0..7 the final implemented bundle of the low
canonical half is:

```
source = (VRN << 61) | ((1 << 50) - 16)
next   = (VRN << 61) | (1 << 50)   # unimplemented within this region
```

A successfully retired nonbranching slot-2 instruction at `source`
may take the architectural post-result UIA **TRAP** at 0x5e00. The source
appears in CR.IIPA and ISR.ei=2; the invalid sequential `next` is
CR.IIP with saved IPSR.ri=0. PSR.ss may contribute ISR.code=8, while
PSR.tb and PSR.lp cannot be invented for a sequential nonbranch.

## QEMU translation bug found by the matrix

The first exact-merge Actions run (41181266207) assembled all 20 guests
and passed region 0–5 but failed all four region 6/7 cases (16/20).
The retained QEMU logs repeatedly reached the **0x400 instruction-TLB
miss vector** after entry at the high virtual source PC. Investigation
found that `ia64_cpu_tlb_fill()` applied legacy Linux region-6 uncached
and region-7 linear-map shortcuts **before** consulting guest-installed
ITR/ITC mappings, forcing these addresses to huge physical addresses.

The fix defers those compatibility direct maps until after the normal
instruction/data translation-register and TLB searches. **Explicit
mappings and their access rights take priority**; when no entry matches,
the historical Linux direct-map fallback remains intact. Physical-mode
fetches still use the normal existing physical-mode translation path.
This is a limited ordering correction; it does not remove the bring-up
shortcuts or claim complete address translation. It also makes explicit
data translations win over the same shortcuts.

A fail-closed source test checks the lookup/fallback ordering alongside
the real QEMU guests. Full post-fix runtime validation is still required.

## Executable coverage

- Retains the eleven region-0 real-IVT guests, existing 130-case policy
  oracle, and inherited QEMU branch, RFI, FP, SS, LP, IFS suites.
- Adds region 1–7 **actual execution** via a 16KiB ITC.I mapping of
  the edge bundle with full 64-bit CR.IFA, and a 16MiB identity
  mapping of the interrupt vector table and guest harness.
- Varies PSR.ss, PSR.tb, PSR.ic and the final I/B/MLX instruction
  family across regions; a source-register write must commit.
- Adds VRN6 and VRN7 near-edge negative controls: valid fallthrough
  with PSR.ss=1 must reach 0x6000, not 0x5e00.
- Python source-independent address assertions run before assembler
  execution. Actual QEMU cases verify the precise vector, source EI,
  CR.IIPA, CR.IIP, IPSR.ri, ISR.code, committed r10 and a single RFI.
- Qualify from the exact **PR merge ref** in CI, including a clean
  qemu-system-ia64 build and retained guest/ELF/log/hash artifact.
  Keep this PR draft until those gates pass.

## Deliberate limitations

This verifies the **low-half canonical edge in all eight virtual
regions**. It is not the 16TiB physical limit, the upper sign-extended
half, region-translation/permission fault precedence, IA-32 transitions,
nested RSE, or firmware boot. Keep issues #21/#45/#27 open and
`ISA COMPLETE=NO`. Do not merge out of stacked order.
