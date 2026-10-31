# Merced upper sign-extended virtual addresses: SS, instruction fetch and RFI UIA

Directly stacked on PR #53. Focus is real, firmware-free, GNU-assembled
guest qualification of **three distinct cases**; this is not full MMU,
RSE, PAL/SAL or Itanium ISA completion.

## Reference and CPU family

- Original Intel *Itanium Processor Reference Manual*, §3.2: the first
  Itanium (Merced) implements 51 bits within each virtual region, three
  VRN bits and 44 physical address bits.
- Intel *Itanium Architecture SDM*, Vol. 2 rev. 2.3, §§4.1, 4.3.2–4.3.3,
  5.3 and interruption vectors 0x400 / 0x5e00 / 0x6000. The 2010
  architectural minimum differs from the original processor; this
  emulator explicitly advertises CPU family 7 and models Merced widths.
- An implemented VA has VRN in [63:61] and **within-region [60:51] equal
  to bit 50**. Both a low half and an upper sign-extended half exist in
  *each* region; the space between them is unimplemented.

For all eight VRNs, choose:

```text
high_base = (1 << 61) - (1 << 50)
high_source = (VRN << 61) | (high_base + 0x3ff0)  # final bundle of 16KiB
high_next   = (VRN << 61) | (high_base + 0x4000)  # valid, but not mapped
bad_low_gap = (VRN << 61) | (1 << 50)             # noncanonical
bad_high_gap= (VRN << 61) | (high_base - 16)      # noncanonical
```

## Actual guest checks and expected interruption state

**Mapped, valid high-half VA:** eight region-0–7 profiles install a real
16KiB ITC.I translation of `high_source` and a 16MiB identity mapping
for the IVT/harness. A real native `rfi` enters at RI2 (RI1 for MLX);
the final instruction commits and triggers **0x6000 PSR.ss**, even though
the *next* valid 16KiB page is deliberately unmapped. The handler
checks `CR.IIPA=source`, `CR.IIP=next`, `IPSR.ri=0`, `ISR.ei=2`,
only SS in `ISR.code` (never TB), NI when `PSR.ic=0`, committed r10,
then repairs the saved IIP and performs one `rfi`.

**Unmapped, valid high-half VA:** four profiles omit only the edge-page
ITC.I while retaining the IVT/harness translation. `PSR.it=1`,
`PSR.ic=1`, `PSR.ss=1`. Fetch must take a **pre-result instruction
TLB miss (0x400)** before the source instruction executes or generates
SS. The handler checks `CR.IFA=source`, `CR.IIP=source`, saved
`IPSR.ri=2` (1 for MLX), modeled `ISR.x/r` flags (bits 32/34) with
`ISR.ei` and no SS/TB code, unchanged r10, followed by one repaired
`rfi`. This is a **mapped-vs-unmapped fault-priority test**, not an
assertion that every possible fetch fault or access-rights case works.

**Unimplemented, noncanonical VA:** four RFI profiles set restored
`PSR.it=1` and attempt both gaps in representative regions 0/1/6/7,
with IC/TB/SS combinations. The model's architecturally selected
post-result **UIA TRAP (0x5e00)** must occur at the `rfi`, *before*
any attempted target fetch. The handler checks source EI/IIPA, preserved
noncanonical target IIP/RI, ISR.code=0 and NI/IC behavior. It repairs
the target and clears IT/debug flags before returning to physical
harness code. No fabricated TB, LP or SS cause on RFI.

An independent 162-case C host address-policy oracle (up from 130)
covers the first/last valid high VA and both invalid gaps for each VRN.
Python host oracles assert actual assembled source mappings, source/next
positions, vectors, RFI restoration and fault-versus-trap states.
Existing 20 low-edge UIA and inherited branch/FP/RFI/SS/LP suites must
run on the **same exact PR merged-tree QEMU binary**.

## What remains open

This tranche does **not** qualify Merced's unimplemented-address
*instruction-fetch FAULT alternative* (the emulator chooses UIA TRAP
on taken transfers), direct unimplemented instruction fetch without a
preceding RFI/branch, all ITLB permissions/keys/physical aliases,
nested RSE, IA-32 transitions, firmware boot or a completed ISA.
Keep issues #21/#45/#27 open, retain the ancestor stack and
`ISA COMPLETE=NO`. Do not merge this PR out of order.
