# IA-64 firmware bringup: fix list

This list tracks known correctness gaps and bringup blockers for the IA-64
firmware path. Items are grouped by status.

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

## Open issues (needs work)

- Firmware now reaches DXE IPL but asserts in `DxeLoad.c` line 790 with a
  negative Status value; need to identify the failing service or missing HOB
  entry (see serial log output).
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
- NaT propagation is incomplete in A-unit integer ops (only padd/psub handle NaT);
  other integer ALU ops ignore NaT and will silently compute values (`target/ia64/translate.c:1173`).
- ALAT/advanced load modeling is minimal; advanced load exceptions and NaT
  handling are not fully implemented (ld.a/chk.a paths in `target/ia64/translate.c`
  and `target/ia64/helper.c`).
- EFI system table scanning is heuristic and may miss firmware layouts; verify
  search ranges for your Flash.fd and consider using firmware-provided pointers
  instead of wide scans (`target/ia64/helper.c:2046`).
- SAL/ESAL Flash volume access assumes a fixed flash base and size; confirm the
  Flash.fd layout matches `IA64_IPF_FW_FLASH_BASE`/`SIZE` or make it dynamic
  (`target/ia64/helper.c:106`).
