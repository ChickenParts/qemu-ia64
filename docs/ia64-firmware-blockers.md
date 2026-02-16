# IA-64 Firmware Blockers

This file is the rolling blocker evidence log referenced by
`IA64_ROADMAP.md`. Keep this focused on current symptoms, repro facts, and
investigation breadcrumbs.

- DXE GCD assert: not observed in current runs after HOB list adjustments; keep an eye on resource HOB consistency if it reappears.
- PEI/DXE hang in byte-copy loop around `pc=0xffe7b1e0` (seen with hang heartbeats). The loop copies a byte and decrements a count loaded via `**(r12+0x30)`; in the failing case the count is a pointer (example store: `pc=0xffe7b2e0` writes `0xffffffff0011b850` to `r12+0x08`), so it takes effectively forever to reach zero.
  - Store watch shows the count slot (`r12+0x08`) being overwritten by `st8 [r31]=r33` at `pc=0xffe24bb0`, which writes a pointer (ex: `0xffffffff0011b898`). That suggests argument/stack-slot corruption or missing initialization before the copy loop.
  - Trace calls to `0xffe24b90` (caller `pc=0xffe24d20`) show `sol=7` and arguments passed in `r39+`; validate `alloc`/CFM and OUT->IN mapping so `r33` contains the expected size rather than a pointer.
- PHIT memory range mismatch: EFI HOB dump shows `mem=[7fffffff1f000000..7fffffff20000000]` (16MiB) even with `-m 512M`; serial shows `Install PeiMemory ... size = 0x1000000`. Investigate why PEI/PHIT is shrinking memory.
- HOB list consistency: confirm the HOB list used by DXE matches the list patched by QEMU (PEI list cloned to 0x3030000). If DXE uses a different list, resource HOB fixes won't be visible.
- FlashMap entries: multiple GUIDed HOBs for FlashMap exist; decode entries to confirm `EFI_FLASH_AREA_EFI_VARIABLES` has correct base/length.
- PEI PPI assert: current run loops in `fw_pei_err` with `EFI_NOT_FOUND` (0x800000000000000e) / `EFI_ABORTED` (0x800000000000001c) around `pc=0xffe268b0..0xffe27400` after `InstallPeiMemory`. Identify which PPI/notify/dispatch path is returning the error; use PPI list/dispatch tracing.
