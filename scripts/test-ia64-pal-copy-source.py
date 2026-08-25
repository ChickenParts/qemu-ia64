#!/usr/bin/env python3
"""Static guard for the IA-64 relocatable PAL implementation."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "target" / "ia64" / "helper.c"
text = SOURCE.read_text(encoding="utf-8")

required = {
    "PAL_COPY_INFO index": "#define IA64_PAL_COPY_INFO       30",
    "PAL_COPY_PAL index": "#define IA64_PAL_COPY_PAL            256",
    "4 KiB buffer": "#define IA64_PAL_COPY_BUFFER_SIZE      0x1000ULL",
    "4 KiB alignment": "#define IA64_PAL_COPY_BUFFER_ALIGN     0x1000ULL",
    "PAL_COPY_INFO case": "case IA64_PAL_COPY_INFO:",
    "PAL_COPY_PAL case": "case IA64_PAL_COPY_PAL:",
    "PAL break bundle word 0": "0x000002000000000aULL",
    "PAL break bundle word 1": "0x0004000000000200ULL",
    "PAL return bundle word 0": "0x0000000100000010ULL",
    "PAL return bundle word 1": "0x0084000080000200ULL",
    "instruction-cache invalidation": "tb_invalidate_phys_range(cs, target_pa,",
    "invalid-argument status": "IA64_PAL_STATUS_INVALID_ARGUMENT",
}

missing = [name for name, needle in required.items() if needle not in text]
if missing:
    for name in missing:
        print(f"missing: {name}", file=sys.stderr)
    raise SystemExit(1)

copy_info = text.index("case IA64_PAL_COPY_INFO:")
copy_pal = text.index("case IA64_PAL_COPY_PAL:")
next_case = text.index("case IA64_PAL_LOGICAL_TO_PHYSICAL:", copy_pal)
if not copy_info < copy_pal < next_case:
    raise SystemExit("PAL copy cases are not ordered as expected")

copy_region = text[copy_info:next_case]
for needle in (
    "a1 == 0 && a2 == 0 && a3 == 0",
    "a1 == 1",
    "a3 > 1",
    "a2 < IA64_PAL_COPY_BUFFER_SIZE",
    "target_pa & (IA64_PAL_COPY_BUFFER_ALIGN - 1)",
    "ia64_fw_write_phys(target_pa, code, sizeof(code))",
    "v0 = IA64_PAL_COPY_PROC_OFFSET",
):
    if needle not in copy_region:
        raise SystemExit(f"PAL copy contract lost required check: {needle}")

print("IA-64 PAL copy source guard: PASS")
