#!/usr/bin/env python3
"""Validate the architecture-critical fields of BOOTIA64.EFI."""

from __future__ import annotations

import pathlib
import struct
import sys

MACHINE_IA64 = 0x0200
PE32_PLUS = 0x020B
EFI_APPLICATION = 0x000A
BASE_RELOCATION_DIRECTORY = 5


def unpack(fmt: str, image: bytes, offset: int) -> tuple[int, ...]:
    size = struct.calcsize(fmt)
    if offset < 0 or offset + size > len(image):
        raise ValueError(f"truncated field at file offset 0x{offset:x}")
    return struct.unpack_from(fmt, image, offset)


def verify(path: pathlib.Path) -> None:
    image = path.read_bytes()
    if len(image) < 0x100:
        raise ValueError("image is too small to contain DOS and PE headers")
    if image[0:2] != b"MZ":
        raise ValueError("missing MZ signature")

    (pe_offset,) = unpack("<I", image, 0x3C)
    if image[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise ValueError(f"missing PE signature at e_lfanew 0x{pe_offset:x}")

    coff = pe_offset + 4
    machine, section_count = unpack("<HH", image, coff)
    optional_size, characteristics = unpack("<HH", image, coff + 16)
    if machine != MACHINE_IA64:
        raise ValueError(
            f"COFF machine is 0x{machine:04x}; expected IA-64 0x0200"
        )
    if section_count != 3:
        raise ValueError(f"expected 3 sections, found {section_count}")
    if (characteristics & 0x0002) == 0:
        raise ValueError("COFF image is not marked executable")

    optional = coff + 20
    if optional + optional_size > len(image):
        raise ValueError("optional header extends past end of image")
    (magic,) = unpack("<H", image, optional)
    if magic != PE32_PLUS:
        raise ValueError(f"optional-header magic is 0x{magic:04x}, not PE32+")

    (entry_rva,) = unpack("<I", image, optional + 16)
    (section_alignment,) = unpack("<I", image, optional + 32)
    (file_alignment,) = unpack("<I", image, optional + 36)
    (size_of_image,) = unpack("<I", image, optional + 56)
    (size_of_headers,) = unpack("<I", image, optional + 60)
    (subsystem,) = unpack("<H", image, optional + 68)
    (directory_count,) = unpack("<I", image, optional + 108)

    if entry_rva == 0 or entry_rva >= size_of_image:
        raise ValueError("entry PLABEL RVA is zero or outside SizeOfImage")
    if section_alignment != 0x1000 or file_alignment != 0x1000:
        raise ValueError(
            "IA-64 smoke image must use 4 KiB section and file alignment"
        )
    if subsystem != EFI_APPLICATION:
        raise ValueError(
            f"subsystem is 0x{subsystem:04x}; expected EFI application 0x000a"
        )
    if directory_count <= BASE_RELOCATION_DIRECTORY:
        raise ValueError("base-relocation data directory is absent")

    reloc_directory = optional + 112 + BASE_RELOCATION_DIRECTORY * 8
    reloc_rva, reloc_size = unpack("<II", image, reloc_directory)
    if reloc_rva == 0 or reloc_size < 12:
        raise ValueError("base-relocation directory is empty or too small")

    section_table = optional + optional_size
    names: set[str] = set()
    entry_is_data = False
    reloc_is_reloc = False
    for index in range(section_count):
        header = section_table + index * 40
        if header + 40 > len(image):
            raise ValueError("section table is truncated")
        name = image[header : header + 8].split(b"\0", 1)[0].decode(
            "ascii", errors="replace"
        )
        virtual_size, virtual_address, raw_size, raw_offset = unpack(
            "<IIII", image, header + 8
        )
        names.add(name)

        mapped_size = max(virtual_size, raw_size)
        if virtual_address <= entry_rva < virtual_address + mapped_size:
            entry_is_data = name == ".data"
        if virtual_address <= reloc_rva < virtual_address + mapped_size:
            reloc_is_reloc = name == ".reloc"
        if raw_size and raw_offset + raw_size > len(image):
            raise ValueError(f"section {name} extends past end of file")

    if names != {".text", ".reloc", ".data"}:
        raise ValueError(f"unexpected section set: {sorted(names)}")
    if not entry_is_data:
        raise ValueError("IA-64 entry RVA must name the PLABEL in .data")
    if not reloc_is_reloc:
        raise ValueError("base-relocation directory does not point into .reloc")
    if size_of_headers == 0 or size_of_headers > len(image):
        raise ValueError("invalid SizeOfHeaders")

    print(
        f"verified {path}: IA-64 PE32+ EFI application; "
        f"entry PLABEL RVA=0x{entry_rva:x}, "
        f"reloc RVA=0x{reloc_rva:x}, size=0x{reloc_size:x}"
    )


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} BOOTIA64.EFI", file=sys.stderr)
        return 2
    try:
        verify(pathlib.Path(argv[1]))
    except (OSError, ValueError, struct.error) as exc:
        print(f"verify.py: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
