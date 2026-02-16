# Status Update (2026-02-16)

## Summary
Work is focused on IA-64 firmware bringup and PEI/DXE stability. The most recent change is a small adjustment to the PEI core entry probe in `target/ia64/helper.c`: the r33 fixup for the PEI PPI pointer is now applied only for the startup handoff path (not the SEC handoff path). This keeps the probe from mutating r33 in the SEC handoff case and aligns with the intended data layout in the startup handoff.

## Roadmap Context
- Canonical execution roadmap: `IA64_ROADMAP.md` (Q2 2026 plan).
- Detailed blocker evidence log: `docs/ia64-firmware-blockers.md`.
- Longer-form architecture and debt context: `IA64-AUDIT.md`, `docs/ia64-firmware-todo.md`.

## Current Code Change
- `target/ia64/helper.c`: moved the `fw_pei_ppi` -> `r33` fixup into the startup handoff branch of `HELPER(fw_pei_core_entry_probe)`.
  - Diff summary: 8 insertions, 8 deletions; no functional change elsewhere.

## Recent Run Artifacts
- `run.repro.err` shows a fatal unimplemented A-slot instruction:
  - `IA64 UNIMPL: pc=a00000010002e240 ri=2 insn=14000000202 A-slot`
- `run.watch.err` shows a fatal unimplemented M-slot instruction:
  - `IA64 UNIMPL: pc=a00000010114d4a0 ri=0 insn=01018202830 M-slot`
- `run.repro.out`/`run.watch.out` indicate the kernel entry handoff and reset proceed before hitting the UNIMPL.

## Known Blockers / Open Issues
(From `docs/ia64-firmware-blockers.md`)
- PEI/DXE hang in byte-copy loop around `pc=0xffe7b1e0`, likely due to argument/stack-slot corruption (size becomes a pointer).
- PHIT memory range mismatch: HOB shows 16MiB even with `-m 512M`.
- HOB list consistency: DXE might be using a different list than the one patched by QEMU.
- FlashMap GUIDed HOB validation needed for EFI variables region.
- PEI PPI assert: `fw_pei_err` loops on `EFI_NOT_FOUND`/`EFI_ABORTED` after `InstallPeiMemory`.

## Suggested Next Steps
1. Decide whether to address the A-slot/M-slot UNIMPLs first (likely needed for forward progress). Use the PCs in `run.repro.err` and `run.watch.err` to decode the exact opcodes and implement or stub them.
2. If focusing on PEI PPI asserts, add/enable PPI dispatch tracing around the `fw_pei_err` loop to identify the failing PPI/notify path.
3. Re-check PHIT memory size and HOB list cloning logic to confirm the firmware sees the expected memory map.

## Files Touched / Useful References
- Modified: `target/ia64/helper.c`
- Logs: `run.repro.err`, `run.repro.out`, `run.watch.err`, `run.watch.out`
- Blocker tracking: `docs/ia64-firmware-blockers.md`
- Overall audit context: `IA64-AUDIT.md`
