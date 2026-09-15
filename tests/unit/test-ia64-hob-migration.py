#!/usr/bin/env python3
"""Synthetic regression tests for analyze-ia64-hob-migration.py."""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import struct
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "analyze-ia64-hob-migration.py"
SPEC = importlib.util.spec_from_file_location("ia64_hob_migration", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def header(hob_type: int, length: int) -> bytes:
    return struct.pack("<HHI", hob_type, length, 0)


def fv(base: int, length: int) -> bytes:
    return header(MODULE.HOB_FV, 24) + struct.pack("<QQ", base, length)


def make_dump(
    dump_base: int,
    list_offset: int,
    entries: list[bytes],
    capacity: int = 0x4000,
) -> MODULE.Dump:
    list_address = dump_base + list_offset
    handoff = bytearray(MODULE.HOB_HANDOFF_SIZE)
    struct.pack_into("<HHI", handoff, 0, MODULE.HOB_HANDOFF, 56, 0)
    struct.pack_into("<II", handoff, 8, 9, 0)
    body = b"".join(entries)
    end_address = list_address + len(handoff) + len(body)
    free_bottom = end_address + MODULE.HOB_HEADER_SIZE
    free_top = dump_base + capacity
    struct.pack_into(
        "<QQQQQ",
        handoff,
        16,
        free_top,
        dump_base,
        free_top,
        free_bottom,
        end_address,
    )
    image = bytearray(capacity)
    cursor = list_offset
    image[cursor : cursor + len(handoff)] = handoff
    cursor += len(handoff)
    image[cursor : cursor + len(body)] = body
    cursor += len(body)
    image[cursor : cursor + 8] = header(MODULE.HOB_END, 8)
    return MODULE.Dump(dump_base, pathlib.Path("synthetic.bin"), bytes(image))


def test_missing_fv_is_restored() -> None:
    source = make_dump(
        0xFFFF0000,
        0x7000,
        [fv(0xFF000000, 0x00800000), fv(0xFF800000, 0x00800000)],
        0x10000,
    )
    target = make_dump(0x040E0000, 0x1000, [header(3, 8)], 0x20000)
    source_list = MODULE.choose_source(MODULE.scan_lists(source))
    target_list = MODULE.choose_target(MODULE.scan_lists(target))
    additions = MODULE.missing_fv_hobs(source_list, target_list)
    assert [entry.fv_identity for entry in additions] == [
        (0xFF000000, 0x00800000),
        (0xFF800000, 0x00800000),
    ]

    repaired = MODULE.repaired_target(target_list, additions)
    repaired_dump = MODULE.Dump(target.base, target.path, repaired)
    repaired_list = MODULE.choose_target(MODULE.scan_lists(repaired_dump))
    assert repaired_list.identities == source_list.identities
    assert repaired_list.end_address == target_list.end_address + 48
    assert repaired_list.free_bottom == target_list.free_bottom + 48


def test_existing_fv_is_not_duplicated() -> None:
    record = fv(0xFF000000, 0x01000000)
    source = make_dump(0xFFFF0000, 0x7000, [record], 0x10000)
    target = make_dump(0x040E0000, 0x1000, [record], 0x20000)
    source_list = MODULE.choose_source(MODULE.scan_lists(source))
    target_list = MODULE.choose_target(MODULE.scan_lists(target))
    assert MODULE.missing_fv_hobs(source_list, target_list) == []
    assert MODULE.repaired_target(target_list, []) == target.data


def test_cli_report() -> None:
    source = make_dump(0xFFFF0000, 0x7000, [fv(0xFF000000, 0x100000)], 0x10000)
    target = make_dump(0x040E0000, 0x1000, [], 0x20000)
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        source_path = root / "source.bin"
        target_path = root / "target.bin"
        report_path = root / "report.json"
        repaired_path = root / "repaired.bin"
        source_path.write_bytes(source.data)
        target_path.write_bytes(target.data)
        result = MODULE.main(
            [
                "--source",
                f"0xffff0000:{source_path}",
                "--target",
                f"0x040e0000:{target_path}",
                "--json",
                str(report_path),
                "--emit-repaired",
                str(repaired_path),
            ]
        )
        assert result == 0
        assert report_path.is_file()
        assert repaired_path.is_file()
        assert "permanent-list-missing-fv-hobs" in report_path.read_text()


if __name__ == "__main__":
    test_missing_fv_is_restored()
    test_existing_fv_is_not_duplicated()
    test_cli_report()
