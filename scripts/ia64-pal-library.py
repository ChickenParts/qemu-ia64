#!/usr/bin/env python3
"""Verify and deterministically repack IA-64 PAL reference images."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import sys
import tarfile
import zlib
from pathlib import Path, PurePosixPath
from typing import Any
from zipfile import BadZipFile, ZipFile

BUNDLE_SIZE = 16


class PalLibraryError(RuntimeError):
    """Raised when a PAL archive or catalog fails validation."""


def safe_member_name(name: str) -> str:
    normalized = name.replace("\\", "/")
    path = PurePosixPath(normalized)
    if (path.is_absolute() or len(path.parts) != 1 or
            any(part in ("", ".", "..") for part in path.parts)):
        raise PalLibraryError(f"unsafe archive member name: {name!r}")
    return path.name


def read_zip(path: Path) -> dict[str, bytes]:
    images: dict[str, bytes] = {}
    try:
        with ZipFile(path) as archive:
            for info in archive.infolist():
                if info.is_dir():
                    continue
                name = safe_member_name(info.filename)
                if name in images:
                    raise PalLibraryError(
                        f"duplicate archive member: {name}")
                images[name] = archive.read(info)
    except (BadZipFile, OSError, RuntimeError) as exc:
        raise PalLibraryError(
            f"cannot read ZIP archive {path}: {exc}") from exc
    return images


def read_tar(path: Path) -> dict[str, bytes]:
    images: dict[str, bytes] = {}
    try:
        with tarfile.open(path, mode="r:*") as archive:
            for info in archive.getmembers():
                if not info.isfile():
                    continue
                name = safe_member_name(info.name)
                if name in images:
                    raise PalLibraryError(
                        f"duplicate archive member: {name}")
                stream = archive.extractfile(info)
                if stream is None:
                    raise PalLibraryError(
                        f"cannot read archive member: {name}")
                try:
                    images[name] = stream.read()
                finally:
                    stream.close()
    except (tarfile.TarError, OSError) as exc:
        raise PalLibraryError(
            f"cannot read tar archive {path}: {exc}") from exc
    return images


def read_archive(path: Path) -> dict[str, bytes]:
    if not path.is_file():
        raise PalLibraryError(f"archive does not exist: {path}")
    if path.suffix.lower() == ".zip":
        return read_zip(path)
    return read_tar(path)


def digest(data: bytes) -> dict[str, Any]:
    return {
        "size": len(data),
        "crc32": f"{zlib.crc32(data) & 0xffffffff:08x}",
        "sha1": hashlib.sha1(data).hexdigest(),
        "sha256": hashlib.sha256(data).hexdigest(),
        "bundle_aligned": len(data) % BUNDLE_SIZE == 0,
        "bundle_count": len(data) // BUNDLE_SIZE,
    }


def load_catalog(path: Path) -> dict[str, Any]:
    try:
        catalog = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PalLibraryError(f"cannot read catalog {path}: {exc}") from exc
    if catalog.get("schema") != 1:
        raise PalLibraryError(
            f"unsupported catalog schema: {catalog.get('schema')!r}")
    return catalog


def verify_archive_identity(path: Path, catalog: dict[str, Any]) -> None:
    actual = digest(path.read_bytes())
    matches = [
        item for item in catalog.get("archives", [])
        if item.get("size") == actual["size"] and
        item.get("sha256") == actual["sha256"]
    ]
    if not matches:
        raise PalLibraryError(
            f"unknown archive identity: size={actual['size']} "
            f"sha256={actual['sha256']}")


def verify_images(images: dict[str, bytes],
                  catalog: dict[str, Any]) -> list[dict[str, Any]]:
    expected = {item["name"]: item for item in catalog.get("images", [])}
    missing = sorted(set(expected) - set(images))
    extra = sorted(set(images) - set(expected))
    if missing or extra:
        raise PalLibraryError(
            f"archive members differ: missing={missing}, extra={extra}")

    report = []
    for name in sorted(images):
        actual = digest(images[name])
        wanted = expected[name]
        checked = ("size", "crc32", "sha1", "sha256",
                   "bundle_aligned", "bundle_count")
        changed = [key for key in checked if actual[key] != wanted[key]]
        if changed:
            raise PalLibraryError(
                f"member identity mismatch for {name}: {changed}")
        report.append({"name": name, **actual})
    return report


def write_deterministic_tar(path: Path,
                            images: dict[str, bytes]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with tarfile.open(path, mode="w:xz", format=tarfile.PAX_FORMAT,
                          preset=9) as archive:
            for name in sorted(images):
                data = images[name]
                info = tarfile.TarInfo(name)
                info.size = len(data)
                info.mode = 0o644
                info.uid = 0
                info.gid = 0
                info.uname = ""
                info.gname = ""
                info.mtime = 0
                archive.addfile(info, io.BytesIO(data))
    except (tarfile.TarError, OSError) as exc:
        raise PalLibraryError(f"cannot write {path}: {exc}") from exc


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path,
                        help="supplied ZIP or deterministic tar.xz")
    parser.add_argument(
        "--manifest", type=Path,
        default=Path("stuff/Merced_PALs.manifest.json"),
        help="reference catalog (default: %(default)s)")
    parser.add_argument(
        "--repack-output", type=Path,
        help="write the deterministic tar.xz after successful verification")
    parser.add_argument("--json", action="store_true",
                        help="print the verified member report as JSON")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        catalog = load_catalog(args.manifest)
        verify_archive_identity(args.archive, catalog)
        images = read_archive(args.archive)
        report = verify_images(images, catalog)
        if args.repack_output is not None:
            write_deterministic_tar(args.repack_output, images)
            verify_archive_identity(args.repack_output, catalog)
        if args.json:
            print(json.dumps(report, indent=2, sort_keys=True))
        else:
            for item in report:
                print(f"{item['name']}: {item['size']} bytes, "
                      f"{item['bundle_count']} bundles, "
                      f"sha256={item['sha256']}")
            if args.repack_output is not None:
                print(f"wrote verified repack: {args.repack_output}")
    except PalLibraryError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
