# Status Update (2026-02-16)

## Summary
This tranche focused on IA-64 instruction coverage and trap behavior hardening:
- Completed A9 packed average/compare instruction families.
- Completed A10 packed shift+add family.
- Routed `gen_unimpl` through IA-64 illegal-op/general exception handling
  (`0x5400`) instead of host-aborting QEMU.

## Implemented in This Tranche
- `target/ia64/translate.c`
  - Added A9 decode coverage for:
    - `pavg1`, `pavg2`
    - `pavg1.raz`, `pavg2.raz`
    - `pavgsub1`, `pavgsub2`
    - `pcmp1.eq`, `pcmp2.eq`, `pcmp4.eq`
    - `pcmp1.gt`, `pcmp2.gt`, `pcmp4.gt`
  - Added A10 decode coverage for:
    - `pshladd2`, `pshradd2`
  - Kept A-unit routing fix in M/I slots: majors limited to
    `0x8,0x9,0xC,0xD,0xE`.
- `target/ia64/helper.c` / `target/ia64/helper.h`
  - Added helper implementations and declarations for all A9 ops above.
  - Added helper implementations and declarations for A10 `pshladd2/pshradd2`.
  - Changed `HELPER(unimpl)` to raise `IA64_VEC_ILLEGAL_OP` via `ia64_fault()`
    after logging, replacing `cpu_abort()`.
- `target/ia64/cpu.h`
  - Added `IA64_VEC_GENERAL_EXCEPTION` and `IA64_VEC_ILLEGAL_OP`.

## Validation Evidence
Build:
- `ninja -C build -j4` passed.

Runtime smoke:
- `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-firmware.sh`
  - Result: `rc=124` (timeout), no host fatal abort.
- `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-kernel.sh`
  - Result: `rc=137` (killed after timeout path), no host fatal abort.

Logs checked:
- `scratch/ia64_logs/qemu.fw.log`
- `scratch/ia64_logs/qemu.log`
- No `IA64 UNIMPL`/fatal-host-abort lines observed in these fresh logs.

## Remaining Open Work
- I-unit `op=7` multimedia gaps still open (`pmpy*`, `pack2.*`,
  `unpack1/2.*`, `pmin/pmax`, `psad1`, `pshl2/4`, `pshr2/4` forms).
- F-unit coverage remains partial; many encodings still fall through to
  `gen_unimpl("F-slot")`.
- Firmware PEI/DXE blockers remain tracked in `docs/ia64-firmware-blockers.md`.
