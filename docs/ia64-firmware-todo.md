# IA-64 firmware bringup: fix list

This list tracks known correctness gaps and bringup blockers for the IA-64
firmware path. Items are grouped by status.

For quarter-level execution sequencing, see `IA64_ROADMAP.md`.

## Fixed in this pass

- Correct ar.ec EC_CNT extraction in call/return ar.pfs construction (`target/ia64/helper.c:5233`).
- Handle ar.ec bitfield encoding for M-unit mov-to-ar paths (`target/ia64/translate.c:2234`, `target/ia64/translate.c:2746`).
- Add region-bit fallback for firmware reads/writes (plus GUID reads) so EFI/ESAL
  can use region-encoded addresses early (`target/ia64/helper.c:1752`, `target/ia64/helper.c:1766`,
  `target/ia64/helper.c:1930`).
- Add a throttled SAL systab install attempt in TLB fill to avoid waiting on the
  first SAL call (`target/ia64/helper.c:7078`).
- Gate synthetic SAL systab installation in `ipf_init` so it only runs for
  direct -kernel boots (`hw/ia64/ipf.c:2596`).
- Disable SAL systab injection during firmware-only boots to avoid clobbering
  firmware tables (`target/ia64/helper.c:2046`).
- Pass the HOB list base in r28 when booting firmware, matching Xen GFW
  expectations (`hw/ia64/ipf.c:2624`, `hw/ia64/ipf.c:3333`).
- Add a PIIX3 southbridge device on the IPF PCI bus to match legacy platform
  topology expected by firmware (`hw/ia64/ipf.c:2260`).
- Match the xenipf firmware SAL break ABI (static r28..r31) and treat PCI
  arguments as width+CF8 address in the break path
  (`target/ia64/helper.c:9566`, `target/ia64/helper.c:10062`).
- Validate SAL PCI size/type strictly while honoring EFI width codes and
  firmware CF8-style addresses (`target/ia64/helper.c:10062`,
  `target/ia64/helper.c:10261`).
- Default-disable EFI HOB patching to avoid masking firmware/TCG bugs
  (`target/ia64/helper.c:6262`).
- Default-disable firmware memcpy/memset fastpath so translation bugs show up
  (`target/ia64/translate.c:109`, `scripts/run-ia64-firmware.sh:19`).
- Respect ar.rsc.mode=0 (lazy) by skipping eager RSE spills so the PEI core's
  HOB pointer is not clobbered by backing-store writes (`target/ia64/helper.c`).
- Restrict A-unit decode routing in M/I slots to architected A-unit majors
  (`0x8,0x9,0xC,0xD,0xE`), so `major=0xA/0xB` no longer takes the A-unit path
  (`target/ia64/translate.c`).
- Implement A10 `pshladd2` / `pshradd2` packed halfword shift+add operations
  (`target/ia64/translate.c`, `target/ia64/helper.c`, `target/ia64/helper.h`).
- Complete A9 packed average/compare families:
  `pavg*`, `pavg*.raz`, `pavgsub*`, `pcmp*.{eq,gt}`
  (`target/ia64/translate.c`, `target/ia64/helper.c`, `target/ia64/helper.h`).
- Route `gen_unimpl` through IA-64 illegal-op/general exception vector
  (`0x5400`) instead of host aborting QEMU
  (`target/ia64/helper.c`, `target/ia64/cpu.h`).
- Complete I-unit `op=7` core packed shift/pack/unpack families:
  `pshl2/4`, `pshr2/4(.u)` (register + immediate), `pack2.*`, `pack4.sss`,
  `unpack1/2.*`
  (`target/ia64/translate.c`, `target/ia64/helper.c`, `target/ia64/helper.h`).
- Fix NaT propagation for implemented I-unit `op=7` scalar forms
  (`popcnt`, `mux1`, `mux2`, `shl`, `shr/shr.u`)
  (`target/ia64/translate.c`).
- Complete remaining I-unit `op=7` multimedia families:
  `pmpyshr2(.u)`, `pmpy2.{r,l}`, `pmin1.u`, `pmax1.u`, `pmin2`, `pmax2`,
  `psad1`
  (`target/ia64/translate.c`, `target/ia64/helper.c`, `target/ia64/helper.h`).
- Add directed `op=7` bare-kernel selftest harness and runner
  (`scripts/ia64-op7-selftest.S`, `scripts/run-ia64-op7-tests.sh`).
- Add targeted PEI copy-path tracing hooks and summarizer script for the
  `0xffe7b1e0` byte-copy blocker
  (`target/ia64/translate.c`, `target/ia64/helper.c`,
  `scripts/ia64-pei-copy-report.sh`).
- Add pre-trigger copy-path history ring and targeted writer tracker for
  `sp+0x08` setup-count triage (with setup-stage-only trigger to avoid
  zero-count exit false positives)
  (`target/ia64/helper.c`, `target/ia64/translate.c`,
  `scripts/run-ia64-firmware.sh`).
- Add first-bad PEI status provenance probes and a non-trace-gated
  StatusCode `PeiLocatePpi` corrective return path (env-gated via
  `QEMU_IA64_PEI_LOCATE_FIX`, default on)
  (`target/ia64/helper.c`, `target/ia64/translate.c`,
  `scripts/run-ia64-firmware.sh`).
- Add first-bad producer-path tracing (PEI call-history ring, status-transition
  logs, RSE chain + dispatch-state dump) to pinpoint the exact `EFI_END_OF_MEDIA`
  production chain
  (`target/ia64/helper.c`, `scripts/run-ia64-firmware.sh`).
- Add `report_status_code` early-phase soft-fail guard so `EFI_NOT_FOUND` /
  `EFI_END_OF_MEDIA` from status-code reporting does not propagate as fatal
  while PPI DB is still empty (`ppi_end <= 0`)
  (`target/ia64/helper.c`, `scripts/run-ia64-firmware.sh`).
- Add default-on StatusCode semantic fix path and optional-path first-bad
  suppression for unresolved StatusCode report flow, with compatibility knobs
  retained for bisecting
  (`target/ia64/helper.c`, `scripts/run-ia64-firmware.sh`,
  `docs/ia64-environment-variables.md`).
- Seed/repair PEI core HOB pointer slot `+0x470` when missing (including
  relocated core discovery paths) so core-side HOB pointer state does not stay
  null when a validated list is already available via `+0x260`/`+0x478`
  (`target/ia64/helper.c`).
- Add bounded PEI HOB-flow contract tracing and `GetHobList` null-out pointer
  repair path (`QEMU_IA64_PEI_HOB_FLOW_TRACE`, `QEMU_IA64_PEI_HOB_PTR_FIX`,
  `QEMU_IA64_PEI_CREATE_HOB_PTR_GUARD`) to prevent the early
  `pc=0xffe24e80` OOR cascade in baseline runs
  (`target/ia64/helper.c`, `scripts/run-ia64-firmware.sh`,
  `docs/ia64-environment-variables.md`).
- Fix PEI core HOB-slot coherence by seeding missing/invalid core `+0x260` and
  `+0x470` slots from validated peer/cached HOB sources during core discovery
  and `fw_pei_hob_init_fix`, eliminating the early `pc=0xffe24e80`
  `EFI_OUT_OF_RESOURCES` regression even with HOB out-pointer repair disabled
  (`target/ia64/helper.c`).
- Fix tested system-memory HOB coverage synthesis to use current PHIT physical
  bounds so DXE GCD preconditions can be materialized in HOB patch mode
  (`target/ia64/helper.c`, `scripts/run-ia64-firmware.sh`).
- Add bounded null `br.call` target repair for the post-GCD DXE control-flow
  path (`pc=0x1ff4f520`) with per-callsite/target cache and wrapper knobs
  (`target/ia64/helper.c`, `target/ia64/translate.c`, `target/ia64/cpu.h`,
  `target/ia64/helper.h`, `scripts/run-ia64-firmware.sh`).
- Add directed A-unit NaT propagation selftest harness (`add`, `and`,
  `shladd`, `adds`, `pavg2`) and runner
  (`scripts/ia64-nat-selftest.S`, `scripts/run-ia64-nat-tests.sh`).
- Harden RSE `loadrs` / `flushrs` handling to consume/clear `ar.rsc.loadrs`,
  keep `ar.bsp`/`ar.bspstore` coherent, and clamp `loadrs` accounting in
  lazy mode (`target/ia64/helper.c`).
- Add env-gated RSE strict tracing (`QEMU_IA64_RSE_STRICT_TRACE`,
  `QEMU_IA64_RSE_STRICT_TRACE_LIMIT`) on `alloc`, `loadrs`, `flushrs`,
  `set_bspstore`, and `ret_restore` paths (`target/ia64/helper.c`).
- Record FP advanced loads (`ldf*.a`) into ALAT and clean up stale `chk.a`
  comments to match implemented behavior (`target/ia64/translate.c`).
- Add env-gated ALAT trace logging (`QEMU_IA64_ALAT_TRACE`,
  `QEMU_IA64_ALAT_TRACE_LIMIT`) for ALAT record/check/invalidate paths
  (`target/ia64/helper.c`).
- Add directed RSE/ALAT bare-kernel selftests and runners
  (`scripts/ia64-rse-selftest.S`, `scripts/run-ia64-rse-tests.sh`,
   `scripts/ia64-alat-selftest.S`, `scripts/run-ia64-alat-tests.sh`).
- Add core F-slot arithmetic/compare/sign coverage:
  - `fadd.s`, `fsub.s`, `fmpy.s`
  - `fcmp.{eq,lt,le,unord}.s0`
  - `fabs`, `fneg`
  (`target/ia64/translate.c`, `target/ia64/helper.c`,
  `target/ia64/helper.h`).
- Add directed F-slot bare-kernel selftest and runner
  (`scripts/ia64-fslot-selftest.S`, `scripts/run-ia64-fslot-tests.sh`).

## Open issues (needs work)

- Firmware blocker now depends on HOB patch mode:
  - baseline (`QEMU_IA64_EFI_HOB_PATCH` off): still asserts in
    `Core\Dxe\Gcd\Gcd.c` line 1736 (`Descrpt: Found`).
  - HOB patch mode (`QEMU_IA64_EFI_HOB_PATCH=1`): synthetic tested sysmem HOB
    is now present and run progresses past `Gcd.c:1736`; bounded null-call
    repair (`QEMU_IA64_CALL_NULL_FIX=1`) is retained as an opt-in bisect
    guard, but default is now off and the historical null-branch path is not
    reproducing in the current 90s run window.
- StatusCode optional-path handling is now semantic and default-on:
  unresolved StatusCode report flow is treated as optional at
  `report_status_code` return and first-bad capture is skipped for this path.
  Root production semantics in notify/dispatch chain are still not fully solved
  and remain correlated with the baseline DXE `Gcd.c:1736` stop.
- SAL systab injection can still be too late or collide with firmware memory:
  we write a synthetic SST_ into fixed low RAM and inject an EFI config entry
  without reserving/allocating space from firmware (`target/ia64/helper.c:1849`,
  `target/ia64/helper.c:2174`, `hw/ia64/ipf.c:946`).
- Firmware status-code callgate patch uses fixed FV offsets; it is build-specific
  and may not match other Flash.fd variants (`hw/ia64/ipf.c:423`).
- Confirm the hypercall stub at `0x10000080` matches the static ABI and that
  PCI width/address ordering is correct for all firmware calls.
- Extended PCI config (type=1) is not implemented; add ECAM handling if
  firmware uses it.
- F-unit decode remains partial beyond core arithmetic/compare/sign paths;
  many architected F-slot encodings still fall through to
  `gen_unimpl("F-slot")` (`target/ia64/translate.c`).
- ALAT/advanced load modeling is still partial; integer + FP ALAT record/check
  paths are now wired for `ld*.a`/`ldf*.a` + `chk.a`, but advanced-load
  exception/NaT corner cases are not fully modeled
  (`target/ia64/translate.c`, `target/ia64/helper.c`).
- RSE dirty/clean partition tracking is still partial; `loadrs`/`flushrs`
  now consume/clear `loadrs` and maintain BSP/BSPSTORE invariants, but full
  architectural dirty/clean partition behavior is not yet complete.
- EFI system table scanning is heuristic and may miss firmware layouts; verify
  search ranges for your Flash.fd and consider using firmware-provided pointers
  instead of wide scans (`target/ia64/helper.c:2046`).
- SAL/ESAL Flash volume access assumes a fixed flash base and size; confirm the
  Flash.fd layout matches `IA64_IPF_FW_FLASH_BASE`/`SIZE` or make it dynamic
  (`target/ia64/helper.c:106`).

## Firmware provenance notes

- Tenox SDV/i2000 images carry `Z:\KittyHawk\Source\...` PDB paths, including
  `Arch\ia64\Chipset\460GX\AUTOSCAN` and `Products\SoftSur\SAL_A/SAL_B`, plus
  `Platform\Recovery\BigSur`, suggesting the KittyHawk codebase across SDV and
  i2000 releases.
- All Tenox 4MiB BIOS images contain an `_FIT_` table at a common offset; the
  FIT entries include a PEI core (type 0x10) but no BFV (type 0x7e) entry.
- The local EDK tree (`EDK/Sample/Version.env`) reports `EDK_BUILD_VERSION =
  Edk1.06` and `TIANO_RELEASE_VERSION = 0x00080006` (UEFI 2.0A / PI 1.0).
- BIOS release notes show version strings like `W460GXBS2.86E.0130...` and
  `...0117C...`, but the binaries do not contain explicit EDK version strings;
  mapping BIOS version -> EDK release remains open.
