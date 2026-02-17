# Status Update (2026-02-17)

## Summary
This tranche completed the core I-unit `op=7` missing set from the phase plan:
- Added packed shift register/immediate forms (`pshl2/4`, `pshr2/4(.u)`).
- Added pack/unpack forms (`pack2.*`, `pack4.sss`, `unpack1/2.*`).
- Corrected NaT propagation for implemented scalar `op=7` forms (`popcnt`,
  `mux1`, `mux2`, `shl`, `shr/shr.u`).

## Implemented in This Tranche
- `target/ia64/translate.c`
  - Added `op=7` decode/execute coverage for:
    - I8 `pshl2/4` (immediate count)
    - I7 `pshl2/4` (register count)
    - I6 `pshr2/4`, `pshr2/4.u` (immediate count)
    - I5 `pshr2/4`, `pshr2/4.u` (register count)
    - I2 `pack2.uss`, `pack2.sss`, `pack4.sss`
    - I2 `unpack1.{h,l}`, `unpack2.{h,l}`
  - Added NaT propagation for existing `op=7` scalar forms:
    - `popcnt`, `mux1`, `mux2`, `shl`, `shr/shr.u`.
- `target/ia64/helper.c` / `target/ia64/helper.h`
  - Added helper implementations/declarations for:
    - `pack2_uss`, `pack2_sss`, `pack4_sss`
    - `unpack1_h`, `unpack1_l`, `unpack2_h`, `unpack2_l`
    - `pshl2`, `pshl4`, `pshr2`, `pshr4`, `pshr2_u`, `pshr4_u`.

## Validation Evidence
Build:
- `ninja -C build -j4` passed.

Runtime smoke:
- `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-firmware.sh`
  - Result: `rc=124` (timeout), no host abort.
- `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-kernel.sh`
  - Result: `rc=137` (killed after timeout path), no host abort.

Logs checked:
- `scratch/ia64_logs/qemu.fw.log`
- `scratch/ia64_logs/qemu.log`
- `scripts/ia64-unimpl-report.sh --top 50 ...`
  - Result: no `IA64 UNIMPL` lines found in these fresh logs.

## Remaining Open Work
- I-unit `op=7` remaining families: `pmpy*`, `pmin/pmax`, `psad1`.
- F-unit coverage remains partial; many encodings still fall through to
  `gen_unimpl("F-slot")`.
- Firmware PEI/DXE blockers remain tracked in `docs/ia64-firmware-blockers.md`.
