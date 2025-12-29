# IA-64 Firmware Blockers

- DXE GCD assert: firmware stops with `ASSERT ... Gcd.c Line 1736, Descrpt: Found` in `scratch/ia64_logs/serial.fw.20251229-005330.log`. Likely no resource descriptor matching PHIT memory range.
- PHIT memory range mismatch: EFI HOB dump shows `mem=[7fffffff1f000000..7fffffff20000000]` (16MiB) even with `-m 512M`; serial shows `Install PeiMemory ... size = 0x1000000`. Investigate why PEI/PHIT is shrinking memory.
- HOB list consistency: confirm the HOB list used by DXE matches the list patched by QEMU (PEI list cloned to 0x3030000). If DXE uses a different list, resource HOB fixes won't be visible.
- FlashMap entries: multiple GUIDed HOBs for FlashMap exist; decode entries to confirm `EFI_FLASH_AREA_EFI_VARIABLES` has correct base/length.
