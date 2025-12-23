# IA-64 firmware bringup TODO

## Current blockers
- Firmware asserts in `AfterMemMP.c` line 161 after FakeMemMap; hang at `IP=0xffe737a0`.
- Assertion path: `PeiServices->ReportStatusCode` (ip `0xffe22330`) returns `EFI_NOT_AVAILABLE_YET` (Tiano `0x1c`), causing `ASSERT_EFI_ERROR`.
- `ReportStatusCode` calls `PeiServices->LocatePpi` (ip `0xffe26510`) for `PEI_STATUS_CODE_PPI_GUID` (`229832d3-7a30-4b36-b827-f40cb7d45436`) and gets `EFI_NOT_FOUND` (`0x0e`).
- StatusCode PPI not installed; `InstallPpi` calls captured so far only install:
  SecPlatformInfo/PlatformInfo/PeiFlashMap/BaseMemory/Autoscan/AfterMemMP PPI GUIDs (no StatusCode).
- Flash scan: the StatusCode GUID appears only inside the `PeiMain` PEIM file
  (`ffs_guid=52c05b14-0b98-496c-bc3b-04b50211d680`, UI `PeiMain`), no PEI_DEPEX section.
- Call trace shows `FfsFindSectionData` being used (returns NOT_FOUND for PEI_DEPEX, OK for PE32),
  but `FfsFindNextVolume`/`FfsFindNextFile` are not called in this path.

## Investigation items
- Determine where a StatusCode PPI provider should come from (missing PEIM vs. missing SEC PPI list).
- If the platform is expected to inject a StatusCode PPI, decide the correct callgate/ABI and where to install it.
- Validate that PEI sees all firmware volumes via FV HOBs and that FV 2 (`0xffe20000`) is in the HOB list.
- Audit RSE/register stack handling around `br.call`/`br.ret` to rule out corrupt arguments in PEI services.
- Validate GFW HOB list contents and placement against expected firmware layout.
- Re-check firmware FV layout and DXE core addresses against flash window mapping.

## Logging / tools
- SAL trace: `QEMU_IA64_FW_SAL_TRACE=1` + `QEMU_IA64_FW_LOG=1`.
- Hang dump: `IA64_HANG_ABORT` + `QEMU_IA64_HANG_DUMP_PC=addr`.
- Firmware scan: `QEMU_IPF_FW_SCAN=1` for FV range/DXE core.
- FV parser: `scripts/ia64_dump_fv.py --list --show-sections`.
