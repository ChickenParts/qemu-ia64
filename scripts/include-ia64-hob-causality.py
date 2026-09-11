#!/usr/bin/env python3
"""Add the IA-64 HOB causality source to the owning Meson source set.

This helper is for diagnostic CI worktrees.  It is idempotent and deliberately
modifies only ``hw/ia64/meson.build``; callers restore that file after building.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

SOURCE = "hob-migration-causality.c"


def include_source(text: str) -> str:
    if SOURCE in text:
        return text

    source_sets = re.findall(
        r"(?m)^\s*([A-Za-z_]\w*)\s*=\s*ss\.source_set\(\)\s*$",
        text,
    )
    if not source_sets:
        raise ValueError("no Meson source_set() declaration was found")

    owner: str | None = None
    for name in source_sets:
        if re.search(
            rf"(?m)^\s*hw_arch\s*\+=\s*\{{\s*['\"]ia64['\"]\s*:\s*{re.escape(name)}\s*\}}",
            text,
        ):
            owner = name
            break
    if owner is None and len(source_sets) == 1:
        owner = source_sets[0]
    if owner is None:
        for name in source_sets:
            if re.search(rf"\b{re.escape(name)}\.add\([^\n]*ipf\.c", text):
                owner = name
                break
    if owner is None:
        raise ValueError(
            "could not identify the IA-64 source set from hw_arch or ipf.c"
        )

    insertion = f"{owner}.add(files('{SOURCE}'))\n"
    arch_binding = re.search(
        rf"(?m)^\s*hw_arch\s*\+=\s*\{{\s*['\"]ia64['\"]\s*:\s*{re.escape(owner)}\s*\}}\s*$",
        text,
    )
    if arch_binding is not None:
        return text[: arch_binding.start()] + insertion + text[arch_binding.start() :]

    declaration = re.search(
        rf"(?m)^\s*{re.escape(owner)}\s*=\s*ss\.source_set\(\)\s*$",
        text,
    )
    assert declaration is not None
    end = text.find("\n", declaration.end())
    if end < 0:
        end = len(text)
        separator = "\n"
    else:
        end += 1
        separator = ""
    return text[:end] + separator + insertion + text[end:]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "path",
        nargs="?",
        type=pathlib.Path,
        default=pathlib.Path("hw/ia64/meson.build"),
    )
    arguments = parser.parse_args(argv)

    try:
        original = arguments.path.read_text()
        updated = include_source(original)
        arguments.path.write_text(updated)
    except (OSError, ValueError) as exc:
        print(f"include-ia64-hob-causality.py: {exc}", file=sys.stderr)
        return 1

    if updated.count(SOURCE) != 1:
        print(
            "include-ia64-hob-causality.py: source was not included exactly once",
            file=sys.stderr,
        )
        return 1
    print(f"included {SOURCE} in {arguments.path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
