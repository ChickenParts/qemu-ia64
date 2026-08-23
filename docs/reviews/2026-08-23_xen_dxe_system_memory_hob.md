# Xen/IPF DXE tested-system-memory HOB repair

Date: 2026-08-23
Integration target: `metachicken`

## Failure contract

After the PEI relocation correction, Xen reaches the DXE core but its migrated
HOB list can omit the tested `EFI_RESOURCE_SYSTEM_MEMORY` descriptor covering
PHIT `EfiFreeMemoryBottom..EfiFreeMemoryTop`. DXE GCD initialization searches
for exactly that descriptor and asserts when it is absent.

The historical `QEMU_IA64_EFI_HOB_PATCH` path proves that synthesizing the
missing descriptor clears the GCD boundary, but that path also performs broad
HOB discovery, firmware-volume injection, memory-type insertion, GP-window
reservation, address retagging, and other unrelated repairs.

## Narrow repair

The default-on `QEMU_IA64_PEI_SYSMEM_HOB_FIX` path performs one operation only:

1. wait until PEI permanent memory has been installed;
2. obtain the validated HOB list from the PEI core near the live stack;
3. validate PHIT ranges, the physical RAM envelope, the exact END-HOB pointer,
   and available free-list capacity;
4. accept an existing tested system-memory descriptor when it covers the PHIT
   free range; otherwise
5. replace the END HOB with one 0x30-byte tested, initialized, present,
   write-back-cacheable system-memory descriptor, append a new END HOB, and
   update PHIT `EfiEndOfHobList` and `EfiFreeMemoryBottom`.

It does not scan arbitrary memory, include the IPF slack window, inject firmware
volumes, rewrite memory-type information, or enable the broad HOB patch. Set
`IA64_PEI_SYSMEM_HOB_FIX=0` in the firmware runner for a clean A/B comparison.

## Acceptance gate

The Xen runtime gate requires the canonical retained `Flash.fd` and broad HOB
patching disabled. A passing run must emit `pei_sysmem_hob_fix inserted`, clear
the former `DxeLoad.c:536` and `Gcd.c:1736` assertions, and reach DXE memory
service initialization without a host abort. The next observed assertion or
control-flow boundary becomes a separate feature tranche.
