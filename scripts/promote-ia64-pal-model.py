#!/usr/bin/env python3
"""Promote the modular IA-64 PAL copy model into the large legacy helper."""

from __future__ import annotations

from pathlib import Path

HELPER = Path("target/ia64/helper.c")
MESON = Path("target/ia64/meson.build")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


def update_helper() -> None:
    text = HELPER.read_text(encoding="utf-8")

    if '#include "pal.h"\n' not in text:
        text = replace_once(
            text,
            '#include "cpu.h"\n',
            '#include "cpu.h"\n#include "pal.h"\n',
            "PAL header include",
        )

    constants = (
        "#define IA64_PAL_COPY_INFO       30\n"
        "#define IA64_PAL_COPY_PAL        256\n"
    )
    if "#define IA64_PAL_COPY_INFO" not in text:
        text = replace_once(
            text,
            "#define IA64_PAL_HALT_LIGHT      29\n",
            "#define IA64_PAL_HALT_LIGHT      29\n" + constants,
            "PAL copy procedure constants",
        )

    cases = (
        "    case IA64_PAL_COPY_INFO:\n"
        "        ia64_pal_copy_info(a1, a2, a3, &status, &v0, &v1, &v2);\n"
        "        break;\n"
        "    case IA64_PAL_COPY_PAL:\n"
        "        ia64_pal_copy_pal(env, a1, a2, a3, &status, &v0, &v1, &v2);\n"
        "        break;\n"
    )
    if "case IA64_PAL_COPY_INFO:" not in text:
        text = replace_once(
            text,
            "    case IA64_PAL_VM_PAGE_SIZE:\n",
            cases + "    case IA64_PAL_VM_PAGE_SIZE:\n",
            "PAL copy dispatch",
        )

    text = text.replace(
        "/* Return all-zero feature sets (matches SKI). */",
        "/* This synthetic platform exposes no optional bus-interface features. */",
    )
    text = text.replace(
        "/* SKI returns 4/4 for PAL_DEBUG_INFO. */",
        "/* The synthetic CPU profile exposes four data and four instruction debug registers. */",
    )

    HELPER.write_text(text, encoding="utf-8")


def update_meson() -> None:
    text = MESON.read_text(encoding="utf-8")
    if "  'pal.c',\n" not in text:
        text = replace_once(
            text,
            "  'helper.c',\n",
            "  'helper.c',\n  'pal.c',\n",
            "PAL source list",
        )
    MESON.write_text(text, encoding="utf-8")


def main() -> int:
    update_helper()
    update_meson()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
