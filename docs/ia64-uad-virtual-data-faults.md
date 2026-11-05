# Merced noncanonical virtual data addresses — General Exception qualification

Directly stacked on PR #57. This is the next step after validating
ITC.D, pinned DTR, and pinned ITR page presence/permission faults.

## Architectural oracle

Source: Intel *Itanium Architecture Software Developer's Manual*,
rev. 2.3, Volume 2, §§4.3.2–4.3.3 (pp.2:74–75),
Table 5-3 (fault qualification), §8 (General Exception vector,
ISR.code and interruption-resource validity).

The emulator advertises original Merced (family 7), with **51
implemented within-region virtual bits** and VRN[63:61]. Bits
VA[60:51] must all replicate VA[50]. Within every VRN, the address
immediately above the low positive half, and the address immediately
below the high sign-extended half, are **noncanonical/unimplemented**.

- **Ordinary non-speculative load/store** to an unimplemented data
  virtual address raises **Unimplemented Data Address** through the
  **General Exception vector 0x5400**, *before* a DTLB miss,
  Page Not Present, AR, Access Bit, Dirty Bit, or ordinary Linux
  compatibility direct-map fallback.
- **ISR.code{7:4} = 3** (0x30), **ISR.rs=0** for ordinary (non-RSE)
  data references; read sets ISR.r and write sets ISR.w. The fault
  restarts at its original instruction slot and must not commit a
  load destination or a store into physical memory.
- Table 8-1 marks **CR.IFA unspecified** for this vector. Do not
  inspect CR.IFA as an architectural correctness oracle for UAD.
- The instruction-family-specific behavior is different for
  speculative loads (NaT), non-faulting probes/prefetches and
  mandatory/eager RSE; these are **not** generalized by this
  non-speculative load/store test scope.

The high-level address table is independently calculated per VRN:

```text
VRN base           = VRN << 61
invalid_low_gap    = (VRN << 61) | (1 << 50)
high_canonical_min = (VRN << 61) | ((1 << 61) - (1 << 50))
invalid_high_gap   = high_canonical_min - 0x4000
```

The negative cases deliberately use 16 KiB-aligned operands,
preventing an unrelated unaligned-data exception from taking priority.

## Production change

In `target/ia64/helper.c`, `ia64_cpu_tlb_fill()` now tests an
incoming **data** address when `PSR.dt=1` against the already-reviewed
Merced `ia64_uia_virtual_implemented()` sign-extension oracle.
If invalid, it enters `ia64_exception()` with vector **0x5400**,
`code=0x30`, no trap, and read/write access qualifiers. The check
is before firmware and percpu compatibility mappings, physical-mode
selection, explicit DTR/DTC lookup, and region-6/7 direct maps.

This *does not* change instruction fetch (which already has its own
branch/RFI UIA and ITLB rules), physical-mode addressing, or the
existing valid virtual memory translation/permission path. It also
does not synthesize CR.IFA, whose value is architecturally unspecified
for this vector.

## Real GNU IA-64 qualification

Extend the existing `scripts/run-ia64-dtlb-fault-priority-tests.py`
**24 → 42** firmware-free real-IVT guests:

- **16** actual noncanonical PSR.dt=1 operands: both gaps for all
  eight VRNs, alternating an ordinary M-slot `ld8` and `st8`.
  Even the low-gap region-6/7 operands that match QEMU's historical
  direct-map shortcut must fault before the shortcut is applied.
- **2** additional valid upper-half controls: region-6 high-half
  load and region-7 high-half store through real ITC.D mappings.
  Both must commit, avoiding an overbroad noncanonical classification.
- **24** inherited real DTC/DTR guest cases, with the existing
  missing/rights/access/dirty/not-present positive and negative
  controls preserved.

Each fault handler tests exact vector, ISR.code{7:4}=3, ISR.r/w,
saved source CR.IIP/IPSR.ri and saved PSR.dt/ic,
uncommitted load register or physical store and a repaired native
`rfi`. The handler clears live PSR.dt with `rsm`+`srlz.d`
before physical data verification, while verifying the saved IPSR
retains DT/ic. **CR.IFA is explicitly not compared.**

Independent Python oracles verify 8 VRNs, both canonical gaps,
read/write selection, uninterrupted valid high-half controls, and
that the early production check precedes every compatibility
shortcut. Existing CI must GNU-assemble all 42, clean-build
`qemu-system-ia64`, run 42 DTLB/UAD plus 12 pinned ITR,
32 sequential UIA, 20 branch UIA, 15 RFI UIA, 20 SS, 17 LP,
5 FP+SS, and 12 IFS/RFI **on one exact PR-merge executable**
(**175 real guest executions**).

## Deliberate limitations

- The general MMU refill path does not itself distinguish all
  speculative loads and non-access references; those operations
  require dedicated architectural handling and validation. This
  tranche's proven scope is **ordinary non-speculative ld8/st8**.
- Physical-mode unimplemented-address checks, faulting
  `lfetch/probe`, and unimplemented addresses supplied to
  `itr/itc/ptr` are separate tests.
- IA-64 protection keys/PKR, NaTPage, VHPT, mandatory RSE
  unimplemented addresses, IA-32 and firmware remain open.
- Preserve the stacked PR ancestry and all #21/#45/#27 issues.
  `ISA COMPLETE=NO`.
