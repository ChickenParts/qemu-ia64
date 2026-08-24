#!/usr/bin/env python3
"""Directed tests for ia64-xen-artifact-inventory.py."""

from __future__ import annotations

import hashlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "ia64-xen-artifact-inventory.py"
spec = importlib.util.spec_from_file_location("ia64_xen_artifact_inventory", MODULE_PATH)
assert spec is not None and spec.loader is not None
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)


def make_zip(members: dict[str, bytes]) -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for name, data in members.items():
            zf.writestr(name, data)
    return output.getvalue()


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    check(module.is_firmware_candidate("tree/binaries/Flash.fd", 16),
          "Flash.fd must be admitted")
    check(module.is_firmware_candidate("out/gfw-ia64.rom", 16),
          "GFW ROM must be admitted")
    check(module.is_firmware_candidate("binaries/odd-name.bin", 16),
          "historical binaries directory must be admitted")
    check(not module.is_firmware_candidate("logs/serial.log", 16),
          "ordinary logs must not be hashed as firmware")
    check(not module.is_firmware_candidate("Flash.fd", 0),
          "empty placeholders must be rejected")

    canonical = b"canonical-xen-ia64-firmware"
    canonical_sha = hashlib.sha256(canonical).hexdigest()
    archive = make_zip({
        "logs/qemu.log": b"not firmware",
        "payload/binaries/Flash.fd": canonical,
        "payload/gfw-old.rom": b"different image",
    })
    candidates, exact = module.scan_artifact_zip(
        archive,
        artifact_id=42,
        artifact_name="ia64-xen-runtime",
        canonical_sha256=canonical_sha,
        max_member_bytes=1024,
    )
    check(len(candidates) == 2, "exactly two plausible members must be hashed")
    check(exact == canonical, "the exact canonical bytes must be returned")
    matches = [candidate for candidate in candidates if candidate.exact_match]
    check(len(matches) == 1, "exactly one member must match the canonical hash")
    check(matches[0].member == "payload/binaries/Flash.fd",
          "the exact member provenance must be preserved")

    oversized, exact = module.scan_artifact_zip(
        make_zip({"Flash.fd": b"12345"}),
        artifact_id=7,
        artifact_name="ia64-firmware",
        canonical_sha256=hashlib.sha256(b"12345").hexdigest(),
        max_member_bytes=4,
    )
    check(not oversized and exact is None,
          "members above the configured cap must be skipped")

    result = {
        "repository": "ChickenParts/qemu-ia64",
        "canonical_sha256": canonical_sha,
        "artifacts_seen": 1,
        "artifacts_selected": 1,
        "candidates": [candidate.as_dict() for candidate in candidates],
        "exact_match": matches[0].as_dict(),
        "errors": [],
    }
    report = module.markdown_report(result)
    check("Canonical image recovered" in report,
          "report must identify a recovered canonical image")
    check("payload/binaries/Flash.fd" in report,
          "report must preserve the matching member path")

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "report.md"
        path.write_text(report, encoding="utf-8")
        check(path.stat().st_size > 0, "report must be writable")

    print("IA-64 Xen artifact inventory tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
