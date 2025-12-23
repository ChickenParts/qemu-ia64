# IA-64 firmware bringup TODO

## Current blockers
- Firmware asserts in `AfterMemMP.c` line 161 after FakeMemMap; hang at `IP=0xffe737a0`.
- Assertion path: `PeiServices->ReportStatusCode` (ip `0xffe22330`) returns `EFI_NOT_AVAILABLE_YET` (Tiano `0x1c`), causing `ASSERT_EFI_ERROR`.
- `ReportStatusCode` calls `PeiServices->LocatePpi` (ip `0xffe26510`) for `PEI_STATUS_CODE_PPI_GUID` (`229832d3-7a30-4b36-b827-f40cb7d45436`) and gets `EFI_NOT_FOUND` (`0x0e`).
- StatusCode PPI not installed; early `InstallPpi` calls (first 10) include SecPlatformInfo, etc., but not StatusCode.

## Investigation items
- Find why StatusCode PEIM never installs `PEI_STATUS_CODE_PPI_GUID` before `AfterMemMP`.
- Trace `InstallPpi` calls to see if StatusCode is ever attempted; if not, trace PEI dispatcher/FV scan.
- Probe `FfsFindNextVolume`/`FfsFindNextFile`/`FfsFindSectionData` for early errors that could skip StatusCode PEIM.
- Audit RSE/register stack handling around `br.call`/`br.ret` to rule out corrupt arguments in PEI services.
- Validate GFW HOB list contents and placement against expected firmware layout.
- Re-check firmware FV layout and DXE core addresses against flash window mapping.

## Logging / tools
- SAL trace: `QEMU_IA64_FW_SAL_TRACE=1` + `QEMU_IA64_FW_LOG=1`.
- Hang dump: `IA64_HANG_ABORT` + `QEMU_IA64_HANG_DUMP_PC=addr`.
- Firmware scan: `QEMU_IPF_FW_SCAN=1` for FV range/DXE core.
