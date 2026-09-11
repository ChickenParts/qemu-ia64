#!/usr/bin/env python3
"""Inspect IA-64 PEI HOB-list snapshots and diagnose migration loss.

The tool operates on physical-memory dumps made with QEMU's ``pmemsave``.
It never modifies a running guest.  ``--emit-repaired`` writes a copy of the
*target dump* with missing firmware-volume HOBs inserted immediately before
its end marker; this is useful for a controlled debugger-side causality test.

Examples::

    scripts/analyze-ia64-hob-migration.py \
        --source 0xffff0000:firmware-top.bin \
        --target 0x040e0000:permanent-hob-window.bin

    scripts/analyze-ia64-hob-migration.py \
        --source 0xffff0000:firmware-top.bin \
        --target 0x040e0000:permanent-hob-window.bin \
        --emit-repaired permanent-with-fv-hobs.bin --json report.json
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import struct
import sys
from collections.abc import Iterable

HOB_HANDOFF = 0x0001
HOB_FV = 0x0005
HOB_END = 0xFFFF
HOB_HEADER_SIZE = 8
HOB_HANDOFF_SIZE = 56
MAX_LIST_SIZE = 1024 * 1024


class HobError(ValueError):
    """A candidate is not a structurally valid HOB list."""


@dataclasses.dataclass(frozen=True)
class Dump:
    base: int
    path: pathlib.Path
    data: bytes


@dataclasses.dataclass(frozen=True)
class Hob:
    address: int
    hob_type: int
    length: int
    raw: bytes

    @property
    def fv_identity(self) -> tuple[int, int] | None:
        if self.hob_type != HOB_FV or self.length < 24:
            return None
        return struct.unpack_from("<QQ", self.raw, 8)


@dataclasses.dataclass(frozen=True)
class HobList:
    dump: Dump
    offset: int
    end_address: int
    free_top: int
    free_bottom: int
    entries: tuple[Hob, ...]

    @property
    def address(self) -> int:
        return self.dump.base + self.offset

    @property
    def fv_hobs(self) -> tuple[Hob, ...]:
        return tuple(entry for entry in self.entries if entry.hob_type == HOB_FV)

    @property
    def identities(self) -> tuple[tuple[int, int], ...]:
        return tuple(
            identity
            for entry in self.fv_hobs
            if (identity := entry.fv_identity) is not None
        )

    def as_dict(self) -> dict[str, object]:
        return {
            "dump": str(self.dump.path),
            "address": hex(self.address),
            "end_address": hex(self.end_address),
            "free_top": hex(self.free_top),
            "free_bottom": hex(self.free_bottom),
            "types": [hex(entry.hob_type) for entry in self.entries],
            "firmware_volumes": [
                {"base": hex(base), "length": hex(length)}
                for base, length in self.identities
            ],
        }


def integer(text: str) -> int:
    return int(text, 0)


def parse_dump(specification: str) -> Dump:
    try:
        base_text, filename = specification.split(":", 1)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "dump must have BASE:PATH form"
        ) from exc
    path = pathlib.Path(filename)
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    return Dump(integer(base_text), path, data)


def parse_list(dump: Dump, offset: int) -> HobList:
    if offset < 0 or offset + HOB_HANDOFF_SIZE > len(dump.data):
        raise HobError("handoff header is outside the dump")

    hob_type, length, _reserved = struct.unpack_from("<HHI", dump.data, offset)
    if hob_type != HOB_HANDOFF:
        raise HobError("candidate does not begin with a handoff HOB")
    if length < HOB_HANDOFF_SIZE or length % 8:
        raise HobError("invalid handoff HOB length")

    free_top = struct.unpack_from("<Q", dump.data, offset + 32)[0]
    free_bottom = struct.unpack_from("<Q", dump.data, offset + 40)[0]
    end_address = struct.unpack_from("<Q", dump.data, offset + 48)[0]
    address = dump.base + offset
    if end_address < address + length:
        raise HobError("end pointer precedes the handoff HOB")
    if end_address - address > MAX_LIST_SIZE:
        raise HobError("HOB list exceeds the safety limit")

    end_offset = end_address - dump.base
    if end_offset < 0 or end_offset + HOB_HEADER_SIZE > len(dump.data):
        raise HobError("end pointer lies outside the supplied dump")

    cursor = offset
    entries: list[Hob] = []
    for _ in range(4096):
        if cursor + HOB_HEADER_SIZE > len(dump.data):
            raise HobError("truncated HOB header")
        hob_type, length, _reserved = struct.unpack_from(
            "<HHI", dump.data, cursor
        )
        if length < HOB_HEADER_SIZE or length % 8:
            raise HobError("invalid HOB length")
        if cursor + length > len(dump.data):
            raise HobError("HOB extends beyond supplied dump")
        raw = dump.data[cursor : cursor + length]
        entries.append(Hob(dump.base + cursor, hob_type, length, raw))
        if hob_type == HOB_END:
            if cursor != end_offset or length != HOB_HEADER_SIZE:
                raise HobError("end HOB disagrees with the handoff end pointer")
            return HobList(
                dump=dump,
                offset=offset,
                end_address=end_address,
                free_top=free_top,
                free_bottom=free_bottom,
                entries=tuple(entries),
            )
        cursor += length
    raise HobError("HOB list has no end marker")


def scan_lists(dump: Dump) -> list[HobList]:
    results: list[HobList] = []
    for offset in range(0, max(0, len(dump.data) - HOB_HANDOFF_SIZE + 1), 8):
        if struct.unpack_from("<H", dump.data, offset)[0] != HOB_HANDOFF:
            continue
        try:
            candidate = parse_list(dump, offset)
        except (HobError, struct.error):
            continue
        results.append(candidate)
    return results


def choose_source(candidates: Iterable[HobList]) -> HobList:
    with_fv = [candidate for candidate in candidates if candidate.fv_hobs]
    if not with_fv:
        raise HobError("no valid source HOB list contains a firmware-volume HOB")
    return max(with_fv, key=lambda candidate: (len(candidate.fv_hobs), len(candidate.entries)))


def choose_target(candidates: Iterable[HobList]) -> HobList:
    values = list(candidates)
    if not values:
        raise HobError("no valid target HOB list was found")
    # The migrated permanent list is normally the richest low-memory list.
    return max(values, key=lambda candidate: (len(candidate.entries), candidate.address))


def missing_fv_hobs(source: HobList, target: HobList) -> list[Hob]:
    existing = set(target.identities)
    missing: list[Hob] = []
    seen = set(existing)
    for hob in source.fv_hobs:
        identity = hob.fv_identity
        if identity is None or identity[1] == 0 or identity in seen:
            continue
        seen.add(identity)
        missing.append(hob)
    return missing


def repaired_target(target: HobList, additions: Iterable[Hob]) -> bytes:
    additions = list(additions)
    payload = b"".join(hob.raw for hob in additions)
    if not payload:
        return target.dump.data

    target_end_offset = target.end_address - target.dump.base
    new_end = target.end_address + len(payload)
    if new_end + HOB_HEADER_SIZE > target.free_top:
        raise HobError("target HOB heap has insufficient free space")
    if target_end_offset + len(payload) + HOB_HEADER_SIZE > len(target.dump.data):
        raise HobError("supplied target dump does not cover the repaired end HOB")

    output = bytearray(target.dump.data)
    old_end = output[target_end_offset : target_end_offset + HOB_HEADER_SIZE]
    if struct.unpack_from("<HH", old_end)[0:2] != (HOB_END, HOB_HEADER_SIZE):
        raise HobError("target end marker changed during repair")
    output[target_end_offset : target_end_offset + len(payload)] = payload
    new_end_offset = target_end_offset + len(payload)
    output[new_end_offset : new_end_offset + HOB_HEADER_SIZE] = old_end
    struct.pack_into("<Q", output, target.offset + 48, new_end)
    if target.free_bottom >= target.end_address:
        struct.pack_into(
            "<Q",
            output,
            target.offset + 40,
            target.free_bottom + len(payload),
        )
    return bytes(output)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=parse_dump)
    parser.add_argument("--target", required=True, type=parse_dump)
    parser.add_argument("--json", type=pathlib.Path)
    parser.add_argument("--emit-repaired", type=pathlib.Path)
    arguments = parser.parse_args(argv)

    try:
        source = choose_source(scan_lists(arguments.source))
        target = choose_target(scan_lists(arguments.target))
        missing = missing_fv_hobs(source, target)
        report = {
            "source": source.as_dict(),
            "target": target.as_dict(),
            "missing_firmware_volumes": [
                {"base": hex(base), "length": hex(length)}
                for hob in missing
                if (identity := hob.fv_identity) is not None
                for base, length in (identity,)
            ],
            "classification": (
                "permanent-list-missing-fv-hobs" if missing else "no-fv-loss-detected"
            ),
        }
        if arguments.emit_repaired is not None:
            arguments.emit_repaired.write_bytes(repaired_target(target, missing))
            report["repaired_dump"] = str(arguments.emit_repaired)
    except (HobError, OSError, struct.error) as exc:
        print(f"analyze-ia64-hob-migration.py: {exc}", file=sys.stderr)
        return 1

    rendered = json.dumps(report, indent=2)
    print(rendered)
    if arguments.json is not None:
        arguments.json.write_text(rendered + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
