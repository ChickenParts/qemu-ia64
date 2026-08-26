#!/usr/bin/env python3
"""Audit migration of the historical IA-64 IPF.c machine monolith.

The old implementation is a migration ledger, not a behavioural oracle.  This
script distinguishes a model that merely exists in QEMU from one actually
wired into hw/ia64, and it never treats a fuzzy source match as proof that a
device is correct.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
from collections import Counter

SOURCE_SUFFIXES = {".c", ".h", ".inc", ".mak", ".meson"}
CALL_RE = re.compile(
    r"\b(qdev_create|qdev_new|qdev_realize(?:_and_unref)?|"
    r"sysbus_create_simple|sysbus_realize(?:_and_unref)?|"
    r"isa_create_simple|isa_create|pci_create(?:_simple)?|pci_qdev_new|"
    r"usb_create|usb_create_simple|serial(?:_mm)?_init|parallel_init|"
    r"rtc_init|i8042_init|ide_[A-Za-z0-9_]*init[A-Za-z0-9_]*|"
    r"fdctrl_init[A-Za-z0-9_]*|qemu_new_nic|net_init_clients|"
    r"pci_register_bus|pci_root_bus_new|pci_bus_new|isa_bus_new|"
    r"qemu_allocate_irqs|qemu_irq_split|memory_region_add_subregion|"
    r"cpu_register_physical_memory|register_ioport_(?:read|write)|"
    r"qemu_register_machine|machine_init)\s*\((.*?)\)", re.S)
STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
FUNC_RE = re.compile(
    r"(?m)^[ \t]*(?:static[ \t]+)?(?:inline[ \t]+)?"
    r"(?:[A-Za-z_][A-Za-z0-9_]*[ \t\n*]+)+"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)[ \t]*\([^;{}]*\)[ \t\n]*\{")
MACRO_RE = re.compile(
    r"(?m)^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*"
    r"(?:BASE|ADDR|ADDRESS|IRQ|VECTOR|PORT|OFFSET|SIZE)[A-Za-z0-9_]*)\s+([^\n/]+)")


def git(root: pathlib.Path, *args: str) -> str:
    return subprocess.check_output(
        ["git", "-C", str(root), *args], text=True,
        stderr=subprocess.DEVNULL)


def find_legacy(root: pathlib.Path) -> tuple[str, str, bytes]:
    candidates = []
    for line in git(root, "rev-list", "--objects", "--all").splitlines():
        parts = line.split(" ", 1)
        if len(parts) != 2 or not re.search(r"(^|/)ipf\.c$", parts[1], re.I):
            continue
        try:
            data = subprocess.check_output(
                ["git", "-C", str(root), "cat-file", "blob", parts[0]])
        except subprocess.CalledProcessError:
            continue
        candidates.append((data.count(b"\n") + 1, len(data), parts[0], parts[1], data))
    if not candidates:
        raise SystemExit("no historical IPF.c blob found in reachable Git history")
    _, _, blob, path, data = max(candidates)
    return blob, path, data


def source_tree(root: pathlib.Path) -> dict[str, str]:
    tree = {}
    for path in root.rglob("*"):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        rel = path.relative_to(root)
        if ".git" in rel.parts or any(part.startswith("build") for part in rel.parts):
            continue
        tree[rel.as_posix()] = path.read_text(encoding="utf-8", errors="replace")
    return tree


def line_at(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def containing_function(functions: list[tuple[int, str]], offset: int) -> str | None:
    found = None
    for start, name in functions:
        if start > offset:
            break
        found = name
    return found


def meaningful(value: str) -> bool:
    return bool(value and len(value) <= 160 and any(c.isalpha() for c in value)
                and not value.startswith("%")
                and value not in {"r", "w", "rb", "wb", "r+b"})


def present(text: str, value: str) -> bool:
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value):
        return re.search(rf"\b{re.escape(value)}\b", text) is not None
    return value in text


def audit(root: pathlib.Path) -> dict:
    blob, old_path, raw = find_legacy(root)
    legacy = raw.decode("utf-8", errors="replace")
    tree = source_tree(root)
    ia64 = {p: text for p, text in tree.items()
            if p.startswith("hw/ia64/") or p.startswith("include/hw/ia64/")}
    other = {p: text for p, text in tree.items() if p not in ia64}
    ia64_text = "\n".join(ia64.values())
    other_text = "\n".join(other.values())

    old_functions = sorted((m.start(), m.group("name")) for m in FUNC_RE.finditer(legacy))
    current_symbols = {}
    for path, text in tree.items():
        for match in FUNC_RE.finditer(text):
            current_symbols.setdefault(match.group("name"), []).append(
                {"path": path, "line": line_at(text, match.start())})

    obligations = []
    seen = set()
    for match in CALL_RE.finditer(legacy):
        call = match.group(1)
        line = line_at(legacy, match.start())
        context = containing_function(old_functions, match.start())
        values = [("call", call)]
        for raw_literal in STRING_RE.findall(match.group(2)):
            try:
                literal = bytes(raw_literal, "utf-8").decode("unicode_escape")
            except UnicodeDecodeError:
                literal = raw_literal
            if meaningful(literal):
                values.append(("literal", literal))
        for kind, value in values:
            key = (kind, value, call, line)
            if key in seen:
                continue
            seen.add(key)
            machine_hits = [p for p, text in ia64.items() if present(text, value)]
            model_hits = [p for p, text in other.items() if present(text, value)]
            status = ("machine-wired" if machine_hits else
                      "model-available" if model_hits else "unresolved")
            obligations.append({
                "kind": kind, "value": value, "legacy_call": call,
                "legacy_line": line, "legacy_function": context,
                "status": status, "machine_paths": machine_hits[:20],
                "model_paths": model_hits[:20],
            })

    functions = []
    for offset, name in old_functions:
        hits = current_symbols.get(name, [])
        functions.append({
            "name": name, "legacy_line": line_at(legacy, offset),
            "status": "exact-symbol" if hits else "unresolved",
            "current_paths": hits,
        })

    macros = []
    for match in MACRO_RE.finditer(legacy):
        name = match.group(1)
        status = ("machine-present" if present(ia64_text, name) else
                  "tree-present" if present(other_text, name) else "unresolved")
        macros.append({"name": name, "value": " ".join(match.group(2).split()),
                       "legacy_line": line_at(legacy, match.start()), "status": status})

    counts = Counter(row["status"] for row in obligations)
    total = len(obligations)
    wired = counts["machine-wired"]
    available = counts["model-available"]
    return {
        "schema": 1,
        "warning": "Structural correspondence is evidence, not architectural proof.",
        "legacy": {
            "git_blob": blob, "repository_path": old_path,
            "sha256": hashlib.sha256(raw).hexdigest(),
            "bytes": len(raw), "lines": raw.count(b"\n") + 1,
        },
        "current": {"ia64_files": sorted(ia64)},
        "summary": {
            "legacy_functions": len(functions),
            "legacy_wiring_obligations": total,
            "obligation_status": dict(sorted(counts.items())),
            "machine_wired_percent": round(100 * wired / total, 1) if total else 100.0,
            "machine_or_model_present_percent":
                round(100 * (wired + available) / total, 1) if total else 100.0,
            "legacy_address_irq_macros": len(macros),
        },
        "functions": functions, "obligations": obligations,
        "address_irq_macros": macros,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    result = audit(args.root.resolve())
    encoded = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
