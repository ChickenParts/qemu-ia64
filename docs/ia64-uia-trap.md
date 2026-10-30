# Merced unimplemented instruction address (UIA), taken-branch trap

Stacked after [PR #49](https://github.com/ChickenParts/qemu-ia64/pull/49).
Intel Itanium SDM rev2.3 Volume 2 §4.3.3, §5.5.3, §8,
UIA/Lower-Privilege Transfer vector 0x5e00 pp.2:205–206, and
Intel Itanium Processor Reference Manual §3.2 (original Merced).

## Explicit implementation choice

The architecture permits either UIA **TRAP** on a branch that transfers
to an unimplemented address, or a subsequent instruction-fetch UIA
**FAULT**. This emulator chooses the TRAP behavior on taken Itanium
branches. For that choice, UIA has higher priority than LP/TB/SS and
uses vector 0x5e00. The ISR.code contains only concurrent LP bit1,
TB bit2, SS bit3, plus source EI and nested interruption NI;
there is no invented UIA cause bit. The saved target is in CR.IIP,
the original branch source in CR.IIPA/ISR.ei, and next IPSR.ri=0.

This CPU model advertises original Itanium family7, for which the
original hardware implements 51 within-region VA bits + 3 VRN
bits (IMPL_VA_MSB=50), and 44 PA bits (IMPL_PA_MSB=43).
The 2010 architecture revision later increased the minimum virtual
address bits; changing the advertised CPU family would require
revisiting this model's PA/VA widths. The physical-mode firmware
32-bit aliases are normalized with the existing instruction-fetch
helper before address-range classification. That is an existing
emulator compatibility behavior rather than a hardware claim.

## Scope and qualification

- Every **actually taken** branch reaches the shared post-commit helper,
  including when PSR.tb and PSR.ss are both disabled. Former TB/SS-only
  gate was wrong for UIA. The helper fast-returns on an implemented
  target with no debug flags.
- Existing B4 indirect/return and B5 call path state is preserved before
  selecting UIA vector; simultaneous TB and/or SS are recorded in ISR
  rather than delivered as a second trap. A return already raising LP
  shares the same vector and concurrent code.
- Real physical-mode guest IVT profiles test bad PA bit44/45 targets,
  source slots0/1/2, UIA with no debug flags or with SS/TB/SS+TB,
  B5 call committed BR/PFS, B4 return, predicated-off branches, and
  valid-target controls; each repairs saved CR.IIP and clears saved
  debug flags before one genuine RFI.
- A portable independently computed host policy checks virtual
  sign-extension for eight regions, physical boundaries, firmware alias
  normalization, ISR source EI/NI, and concurrent LP/TB/SS codes.
- Clean full QEMU rebuild and inherited 36 branch-SS, 20 SS, 17 LP,
  17 TB, 20 B1/B2, 5 FP-SS, 12 IFS/RFI guest executions run on the
  exact PR merge, retaining per-guest raw assembler/ELF/log/hash evidence.

## Deliberate limits

This bounded tranche does **not** implement UIA for rfi-to-bad-target,
sequential slot2 at the top of implemented VA/PA range, IA-32
transitions/br.ia, or interactions with TLB translation/data UIA
semantics. It does not assert that a virtual-mode guest mapping is
independently qualified by this physical-mode real-IVT matrix; the
pure address policy is unit-tested in that mode. Maintain tracking
issues #21/#45 and ISA COMPLETE=NO. No parent PR merge or rewrite.
