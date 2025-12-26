# IA-64 firmware bringup TODO

## Current blockers
- Firmware asserts in `AfterMemMP.c` line 161 after FakeMemMap; hang at `IP=0xffe737a0`.
- Assert data decoded from `fw_break0` buffer: line 161 in
  `C:\project\r5-xen\Platform\IntelSsg\xenipf\Pei\MpInit\AfterMemMP.c`,
  expression `!(((INTN) (Status)) < 0)` (truncated in buffer).
- Status at assert entry appears to be `EFI_OUT_OF_RESOURCES`
  (`r8=0x8000000000000009` at `pc=0xffe73700`).
- `PEI_STATUS_CODE_PPI_GUID` is now installed and `LocatePpi` returns success
  in later traces; `ReportStatusCode` callgate is functional.
- HOB/PHIT parsing still shows inconsistent data in `fw_pei_oor` (bogus mem ranges),
  likely corrupting PEI allocations and triggering out-of-resources.

## Investigation items
- Identify which PEI service returns `EFI_OUT_OF_RESOURCES` before `AfterMemMP` assert.
- Validate HOB list integrity across PEI stages (PHIT fields, free ranges, and list end).
- Validate that PEI sees all firmware volumes via FV HOBs and that FV 2 (`0xffe20000`) is in the HOB list.
- Audit RSE/register stack handling around `br.call`/`br.ret` to rule out corrupt arguments in PEI services.
- Validate GFW HOB list contents and placement against expected firmware layout.
- Re-check firmware FV layout and DXE core addresses against flash window mapping.

## Logging / tools
- SAL trace: `QEMU_IA64_FW_SAL_TRACE=1` + `QEMU_IA64_FW_LOG=1`.
- Hang dump: `IA64_HANG_ABORT` + `QEMU_IA64_HANG_DUMP_PC=addr`.
- Firmware scan: `QEMU_IPF_FW_SCAN=1` for FV range/DXE core.
- FV parser: `scripts/ia64_dump_fv.py --list --show-sections`.
