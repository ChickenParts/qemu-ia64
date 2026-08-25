#!/usr/bin/env python3
"""Validate and describe named IA-64 firmware inputs without filename guessing."""

from __future__ import annotations

import argparse
import hashlib
import json
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass(frozen=True)
class Firmware:
    id: str
    family: str
    path: str
    size: int
    sha256: str


DEFAULTS = {
    "xen": Path("scratch/ia64-xen-firmware/Flash.fd"),
    "sdv-debug-0.99": Path("stuff/EFI 0.99 Debug SDV.bin"),
}
EXPECTED = {
    "xen": {
        "family": "xen-efi-vfirmware",
        "size": 10_485_760,
        "sha256": "e143e85874ad57bad631853d48f0d47b7e7dbe6c41b4e558bbb4ea5b45775513",
    },
    "sdv-debug-0.99": {
        "family": "intel-sdv",
        "size": 4_194_304,
        "sha256": None,
    },
}


def inspect(firmware_id: str, path: Path) -> Firmware:
    if firmware_id not in EXPECTED:
        raise SystemExit(f"unknown firmware id: {firmware_id}")
    if not path.is_file():
        raise SystemExit(f"firmware not found: {path}")

    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    expected = EXPECTED[firmware_id]
    if len(data) != expected["size"]:
        raise SystemExit(
            f"{firmware_id}: expected {expected['size']} bytes, got {len(data)}"
        )
    if expected["sha256"] and digest != expected["sha256"]:
        raise SystemExit(
            f"{firmware_id}: SHA-256 mismatch: expected {expected['sha256']}, "
            f"got {digest}"
        )

    return Firmware(
        id=firmware_id,
        family=str(expected["family"]),
        path=str(path),
        size=len(data),
        sha256=digest,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("firmware_id", choices=sorted(DEFAULTS))
    parser.add_argument("--path", type=Path)
    parser.add_argument("--shell", action="store_true")
    args = parser.parse_args()

    item = inspect(args.firmware_id, args.path or DEFAULTS[args.firmware_id])
    if args.shell:
        print(f"IA64_FIRMWARE_ID={item.id}")
        print(f"IA64_FIRMWARE_FAMILY={item.family}")
        print(f"IA64_FIRMWARE_PATH={item.path}")
        print(f"IA64_FIRMWARE_SIZE={item.size}")
        print(f"IA64_FIRMWARE_SHA256={item.sha256}")
    else:
        print(json.dumps(asdict(item), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
