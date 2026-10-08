#!/usr/bin/env python3
"""Persist restored IA-64 shadow RSE frames in architectural backing store.

A non-local return can reconstruct the live stacked-register window from a
shadow frame. That restored activation may later outlive the remaining shadow
metadata, so the exact frame must also be materialized in guest backing-store
memory. This edit is fail-closed against the reviewed deferred-return source
shape and can be checked without writing.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

OLD = '''                if (exact) {
                    memcpy(&env->r[32], frame->r, sizeof(frame->r));
                    memcpy(&env->nat[32], frame->nat, sizeof(frame->nat));
                    restored_from_shadow = true;
                }
'''

NEW = '''                if (exact) {
                    memcpy(&env->r[32], frame->r, sizeof(frame->r));
                    memcpy(&env->nat[32], frame->nat, sizeof(frame->nat));
                    /*
                     * The architectural activation can outlive this shadow
                     * frame. Keep guest backing-store memory synchronized so
                     * a later non-local return can reload the same window after
                     * the shadow metadata has aged out.
                     */
                    ia64_rse_store_frame(env, bsp, pfs_cfm & 0x7f);
                    restored_from_shadow = true;
                }
'''


def prepare(root: Path) -> tuple[Path, str]:
    path = root / "target/ia64/helper.c"
    if not path.is_file():
        raise RuntimeError(f"missing source file: {path}")

    text = path.read_text()
    if NEW in text:
        raise RuntimeError("backing-store persistence repair already appears applied")
    count = text.count(OLD)
    if count != 1:
        raise RuntimeError(
            "target/ia64/helper.c: expected exactly one shadow restore block, "
            f"found {count}"
        )

    updated = text.replace(OLD, NEW, 1)
    if updated.count("ia64_rse_store_frame(env, bsp, pfs_cfm & 0x7f);") != 1:
        raise RuntimeError("persistence call was not introduced exactly once")
    return path, updated


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--apply", action="store_true")
    parser.add_argument("source_root", nargs="?", default=".")
    args = parser.parse_args(argv)

    root = Path(args.source_root).resolve()
    path, updated = prepare(root)
    if args.apply:
        path.write_text(updated)
        verb = "applied"
    else:
        verb = "would update"

    print(f"{verb} durable IA-64 RSE shadow restore in {path.relative_to(root)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except RuntimeError as exc:
        print(f"apply-ia64-rse-backing-store-persistence.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
