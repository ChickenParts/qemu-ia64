#!/usr/bin/env python3
"""Source-level contract checks for the independent IA-64 PAL copy model."""

from __future__ import annotations

from pathlib import Path

pal = Path("target/ia64/pal.c").read_text(encoding="utf-8")
header = Path("target/ia64/pal.h").read_text(encoding="utf-8")
gateway = Path("target/ia64/pal-gateway.S").read_text(encoding="utf-8")
helper = Path("target/ia64/helper.c").read_text(encoding="utf-8")
meson = Path("target/ia64/meson.build").read_text(encoding="utf-8")

required_pal = (
    "sizeof(ia64_pal_gateway)",
    "IA64_PAL_GATEWAY_ALIGN",
    "address_space_write(&address_space_memory",
    "cpu_flush_icache_range",
    "alloc_size < sizeof(ia64_pal_gateway)",
    "processor >= cpus",
    "IA64_PAL_STATUS_EINVAL",
    "IA64_PAL_STATUS_ERROR",
)
for token in required_pal:
    if token not in pal:
        raise SystemExit(f"missing PAL copy contract: {token}")

for token in ("break.m 0x1000", "br.ret.sptk.many b0"):
    if token not in gateway:
        raise SystemExit(f"missing gateway operation: {token}")

for token in (
    '#include "pal.h"',
    "case IA64_PAL_COPY_INFO:",
    "case IA64_PAL_COPY_PAL:",
    "ia64_pal_copy_info(a1, a2, a3",
    "ia64_pal_copy_pal(env, a1, a2, a3",
):
    if token not in helper:
        raise SystemExit(f"missing helper dispatch: {token}")

if "  'pal.c'," not in meson:
    raise SystemExit("target/ia64/pal.c is not in the Meson source set")
if "matches SKI" in helper or "SKI returns" in helper:
    raise SystemExit("PAL implementation still contains explicit SKI-derived behavior")
if "32768" in pal or "PAL_COPY_BUFFER_SIZE" in pal:
    raise SystemExit("PAL copy size must derive from the project-owned gateway")
if "IA64_PAL_COPY_TYPE_DEFAULT 0" not in header:
    raise SystemExit("PAL copy type contract is not explicit")

print("IA-64 independent PAL copy source contract passed")
