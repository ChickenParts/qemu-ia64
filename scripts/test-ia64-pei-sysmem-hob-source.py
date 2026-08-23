#!/usr/bin/env python3
"""Fail-closed source contract for the narrow Xen PEI sysmem-HOB repair."""

from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
helper = (root / "target/ia64/helper.c").read_text(encoding="utf-8")
runner = (root / "scripts/run-ia64-firmware.sh").read_text(encoding="utf-8")

required = (
    "ia64_fw_pei_sysmem_hob_fix_enabled",
    "QEMU_IA64_PEI_SYSMEM_HOB_FIX",
    "ia64_fw_pei_ensure_tested_sysmem_hob",
    "!env->fw_pei_mem_installed",
    "ia64_fw_find_pei_hob_list",
    "hob_end - hob_base > (1ULL << 20)",
    "end_hob != hob_end - end_header_len",
    "mem_bottom != (1ULL << 20)",
    "mem_top != ram_size",
    "free_bottom < hob_end",
    "free_top > mem_top",
    "EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE",
    "QEMU_ALIGN_UP(new_list_end, 0x20)",
    "validated_end != new_list_end",
    "IA64: pei_sysmem_hob_fix inserted",
)
for token in required:
    if token not in helper:
        raise SystemExit(f"missing narrow sysmem-HOB contract marker: {token}")

if "IA64_PEI_SYSMEM_HOB_FIX" not in runner:
    raise SystemExit("firmware runner does not expose the narrow repair opt-out")

helper_start = helper.index("static bool ia64_fw_pei_ensure_tested_sysmem_hob")
helper_end = helper.index("static void ia64_fw_try_patch_efi_hobs", helper_start)
narrow = helper[helper_start:helper_end]
if "IA64_IPF_FW_SLACK_SIZE" in narrow or "slack_size" in narrow:
    raise SystemExit("narrow repair must not absorb the broad slack-RAM heuristic")
if "ia64_fw_find_hob_list_in_range" in narrow:
    raise SystemExit("narrow repair must not scan arbitrary memory for HOB lists")
if "EFI_HOB_TYPE_FV" in narrow or "MEMTYPE" in narrow:
    raise SystemExit("narrow repair must insert only the tested sysmem descriptor")

call = "ia64_fw_pei_ensure_tested_sysmem_hob(env, cs, stack_phys);"
broad_return = "if (!enabled && !memtype_enabled)"
if helper.index(call) > helper.index(broad_return):
    raise SystemExit("narrow repair is hidden behind the broad HOB-patch gate")

# The descriptor is exactly one 0x30-byte resource HOB followed by one END HOB.
if not re.search(r"descriptor_len\s*=\s*0x30", narrow):
    raise SystemExit("resource descriptor width is not fixed at 0x30")
if narrow.count("EFI_HOB_TYPE_RESOURCE_DESCRIPTOR") < 2:
    raise SystemExit("resource descriptor scan/write contract is incomplete")
if "mem_top - mem_bottom" not in narrow:
    raise SystemExit("descriptor does not cover the PHIT memory range")

print("IA-64 narrow PEI tested-system-memory HOB source contract passed")
