# IA-64 firmware bringup TODO

## Current blockers
- Firmware asserts in `AfterMemMP.c` line 161 after FakeMemMap; hang at `IP=0xffe737a0` with `r8=EFI_END_OF_MEDIA (0x1c)`.
- Call chain at hang (from b0 trace): `0xffe73790 -> 0xffe74720 -> 0xffe70dc0`. Need to identify the callee and failing service.
- SAL/ESAL break ABI uses r28.. for function/args; verify all callers match SKI/SAL spec (esp. PCI config, state info, freq base, update pal).

## Investigation items
- Decode the function at `0xffe70dc0` (plabel call) and identify the service returning `EFI_END_OF_MEDIA`.
- Audit RSE/register stack handling around `br.call`/`br.ret` for pointer corruption or bad frame sizes.
- Validate GFW HOB list contents and placement against expected firmware layout.
- Re-check firmware FV layout and DXE core addresses against flash window mapping.
- Confirm IOSAPIC/MMIO mapping and `0xfee00000` usage in firmware.

## Logging / tools
- SAL trace: `QEMU_IA64_FW_SAL_TRACE=1` + `QEMU_IA64_FW_LOG=1`.
- Hang dump: `IA64_HANG_ABORT` + `QEMU_IA64_HANG_DUMP_PC=addr`.
- Firmware scan: `QEMU_IPF_FW_SCAN=1` for FV range/DXE core.
