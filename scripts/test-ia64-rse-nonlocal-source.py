#!/usr/bin/env python3
"""Source-level guard for exact-target IA-64 non-local return unwinding."""

from pathlib import Path

source = Path("target/ia64/helper.c").read_text(encoding="utf-8")

required = (
    "static unsigned ia64_rse_unwind_to_return_target",
    "env->rse_frames[i].ret_addr == ret_addr",
    "while (env->rse_depth > (uint32_t)match + 1)",
    "unsigned exact_unwind = ia64_rse_unwind_to_return_target(env, b0);",
    '"ret_nonlocal_unwind ip=%016" PRIx64',
    "!exact_unwind && pfs_unwind_enabled",
)
for token in required:
    if token not in source:
        raise SystemExit(f"missing exact-target non-local return contract: {token}")

helper = source.index("static unsigned ia64_rse_unwind_to_return_target")
restore = source.index("void HELPER(ret_restore)", helper)
call = source.index("ia64_rse_unwind_to_return_target(env, b0)", restore)
pfs = source.index("pfs_unwind_enabled", call)
pop = source.index("if (ia64_rse_pop_window(env))", call)

if not helper < restore < call < pfs < pop:
    raise SystemExit("non-local unwind is not ordered before diagnostic PFS fallback and final pop")
if "getenv(\"QEMU_IA64_RET_UNWIND_PFS\")" not in source:
    raise SystemExit("PFS heuristic lost its explicit diagnostic opt-in")

print("IA-64 exact-target non-local return source guard passed")
