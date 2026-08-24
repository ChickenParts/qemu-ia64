#!/usr/bin/env python3
"""Inventory retained GitHub Actions artifacts for the canonical Xen IA-64 GFW.

The historical efi-vfirmware Mercurial endpoint is unreliable, while firmware
bring-up workflows may still retain the exact image as an Actions artifact.
This tool searches only the current repository's unexpired artifacts, hashes
plausible IA-64 firmware members, and materializes an exact SHA-256 match for a
local replay. It never commits or uploads the firmware itself.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import sys
from typing import Any, Iterable
import urllib.error
import urllib.parse
import urllib.request
import zipfile

CANONICAL_SHA256 = (
    "e143e85874ad57bad631853d48f0d47b7e7dbe6c41b4e558bbb4ea5b45775513"
)
DEFAULT_ARTIFACT_RE = r"xen|firmware|dxe|hob|frontier"
FIRMWARE_SUFFIXES = {".fd", ".rom", ".bin", ".efi"}
REDIRECT_STATUS = {301, 302, 303, 307, 308}
USER_AGENT = "qemu-ia64-xen-artifact-inventory"


@dataclasses.dataclass(frozen=True)
class Candidate:
    artifact_id: int
    artifact_name: str
    member: str
    size: int
    sha256: str
    exact_match: bool

    def as_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


def is_firmware_candidate(name: str, size: int) -> bool:
    """Return whether an artifact member plausibly contains IA-64 firmware."""
    if size <= 0:
        return False
    path = PurePosixPath(name)
    base = path.name.lower()
    suffix = path.suffix.lower()
    parts = {part.lower() for part in path.parts}
    if base in {"flash.fd", "gfw.fd", "gfw.rom", "gfw.bin"}:
        return True
    if "gfw" in base and suffix in FIRMWARE_SUFFIXES:
        return True
    if suffix == ".fd":
        return True
    if "binaries" in parts and suffix in FIRMWARE_SUFFIXES:
        return True
    return False


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def scan_artifact_zip(
    archive: bytes,
    *,
    artifact_id: int,
    artifact_name: str,
    canonical_sha256: str,
    max_member_bytes: int,
) -> tuple[list[Candidate], bytes | None]:
    """Hash plausible firmware members in one Actions artifact ZIP."""
    candidates: list[Candidate] = []
    canonical: bytes | None = None
    with zipfile.ZipFile(io.BytesIO(archive)) as zf:
        for info in zf.infolist():
            if info.is_dir() or not is_firmware_candidate(info.filename, info.file_size):
                continue
            if info.file_size > max_member_bytes:
                continue
            with zf.open(info, "r") as member_file:
                data = member_file.read(max_member_bytes + 1)
            if len(data) != info.file_size or len(data) > max_member_bytes:
                continue
            digest = sha256_bytes(data)
            exact = digest.lower() == canonical_sha256.lower()
            candidates.append(
                Candidate(
                    artifact_id=artifact_id,
                    artifact_name=artifact_name,
                    member=info.filename,
                    size=len(data),
                    sha256=digest,
                    exact_match=exact,
                )
            )
            if exact and canonical is None:
                canonical = data
    return candidates, canonical


def artifact_priority(artifact: dict[str, Any]) -> tuple[int, str, int]:
    """Search Xen-specific and newest artifacts before broad firmware logs."""
    name = str(artifact.get("name") or "").lower()
    if "xen" in name and ("dxe" in name or "hob" in name):
        rank = 0
    elif "xen" in name:
        rank = 1
    elif "dxe" in name or "hob" in name:
        rank = 2
    elif "firmware" in name or "frontier" in name:
        rank = 3
    else:
        rank = 4
    return rank, name, -int(artifact.get("id") or 0)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    """Expose GitHub's redirect so credentials are not sent to blob storage."""

    def redirect_request(
        self,
        req: urllib.request.Request,
        fp: Any,
        code: int,
        msg: str,
        headers: Any,
        newurl: str,
    ) -> None:
        return None


class GitHubArtifacts:
    def __init__(self, repository: str, token: str, api_url: str) -> None:
        self.repository = repository
        self.token = token
        self.api_url = api_url.rstrip("/")

    def _request(self, url: str, *, authenticated: bool = True) -> urllib.request.Request:
        headers = {
            "Accept": "application/vnd.github+json",
            "User-Agent": USER_AGENT,
            "X-GitHub-Api-Version": "2022-11-28",
        }
        if authenticated:
            headers["Authorization"] = f"Bearer {self.token}"
        return urllib.request.Request(url, headers=headers)

    def _read_json(self, url: str) -> dict[str, Any]:
        with urllib.request.urlopen(self._request(url), timeout=60) as response:
            return json.load(response)

    def list_all(self) -> list[dict[str, Any]]:
        artifacts: list[dict[str, Any]] = []
        page = 1
        while True:
            url = (
                f"{self.api_url}/repos/{self.repository}/actions/artifacts"
                f"?per_page=100&page={page}"
            )
            payload = self._read_json(url)
            batch = payload.get("artifacts", [])
            if not isinstance(batch, list):
                raise RuntimeError("GitHub artifacts response did not contain a list")
            artifacts.extend(item for item in batch if isinstance(item, dict))
            if len(batch) < 100:
                break
            page += 1
        return artifacts

    def download(self, artifact: dict[str, Any], max_bytes: int) -> bytes:
        """Download through the API, then follow the signed redirect sans token."""
        url = str(artifact.get("archive_download_url") or "")
        if not url:
            raise RuntimeError("artifact has no archive_download_url")
        declared_size = int(artifact.get("size_in_bytes") or 0)
        if declared_size > max_bytes:
            raise RuntimeError(
                f"artifact is {declared_size} bytes, above {max_bytes}-byte cap"
            )

        opener = urllib.request.build_opener(NoRedirect())
        response: Any
        try:
            response = opener.open(self._request(url), timeout=60)
        except urllib.error.HTTPError as exc:
            if exc.code not in REDIRECT_STATUS:
                raise
            location = exc.headers.get("Location")
            exc.close()
            if not location:
                raise RuntimeError("artifact redirect omitted Location")
            signed_url = urllib.parse.urljoin(url, location)
            # The signed object-store URL authenticates itself. Forwarding the
            # GitHub bearer token across hosts causes Azure to reject it.
            response = urllib.request.urlopen(
                self._request(signed_url, authenticated=False), timeout=180
            )

        with response:
            data = response.read(max_bytes + 1)
        if len(data) > max_bytes:
            raise RuntimeError(f"artifact download exceeded {max_bytes}-byte cap")
        return data


def markdown_report(result: dict[str, Any]) -> str:
    lines = [
        "# IA-64 Xen firmware artifact inventory",
        "",
        f"Repository: `{result['repository']}`",
        f"Canonical SHA-256: `{result['canonical_sha256']}`",
        f"Artifacts visible: {result['artifacts_seen']}",
        f"Artifacts selected: {result['artifacts_selected']}",
        f"Artifacts downloaded: {result['artifacts_downloaded']}",
        f"Candidates hashed: {len(result['candidates'])}",
        "",
    ]
    exact = result.get("exact_match")
    if exact:
        lines.extend(
            [
                "## Canonical image recovered",
                "",
                f"- Artifact: `{exact['artifact_name']}` (ID {exact['artifact_id']})",
                f"- Member: `{exact['member']}`",
                f"- Size: {exact['size']} bytes",
                f"- SHA-256: `{exact['sha256']}`",
                "",
            ]
        )
    else:
        lines.extend(
            [
                "## Canonical image not found",
                "",
                "No unexpired selected artifact contained a plausible firmware member "
                "with the canonical hash.",
                "",
            ]
        )

    lines.extend(["## Candidate members", ""])
    if result["candidates"]:
        lines.append("| Artifact | Member | Bytes | SHA-256 | Exact |")
        lines.append("|---|---|---:|---|:---:|")
        for candidate in result["candidates"]:
            lines.append(
                f"| `{candidate['artifact_name']}` | `{candidate['member']}` | "
                f"{candidate['size']} | `{candidate['sha256']}` | "
                f"{'yes' if candidate['exact_match'] else 'no'} |"
            )
    else:
        lines.append("No plausible firmware members were present.")
    lines.append("")

    errors = result.get("errors", [])
    if errors:
        lines.extend(["## Skipped or unreadable artifacts", ""])
        for error in errors:
            lines.append(
                f"- `{error['artifact_name']}` (ID {error['artifact_id']}): "
                f"{error['error']}"
            )
        lines.append("")
    return "\n".join(lines)


def parse_args(argv: Iterable[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", default=os.getenv("GITHUB_REPOSITORY", ""))
    parser.add_argument(
        "--token", default=os.getenv("GH_TOKEN") or os.getenv("GITHUB_TOKEN", "")
    )
    parser.add_argument(
        "--api-url", default=os.getenv("GITHUB_API_URL", "https://api.github.com")
    )
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--canonical-sha256", default=CANONICAL_SHA256)
    parser.add_argument("--artifact-name-regex", default=DEFAULT_ARTIFACT_RE)
    parser.add_argument("--max-artifact-mib", type=int, default=512)
    parser.add_argument("--max-member-mib", type=int, default=128)
    return parser.parse_args(argv)


def main(argv: Iterable[str] | None = None) -> int:
    args = parse_args(argv)
    if not args.repository or "/" not in args.repository:
        raise SystemExit("--repository owner/name is required")
    if not args.token:
        raise SystemExit("--token or GH_TOKEN/GITHUB_TOKEN is required")
    if not re.fullmatch(r"[0-9a-fA-F]{64}", args.canonical_sha256):
        raise SystemExit("--canonical-sha256 must contain 64 hexadecimal digits")
    if args.max_artifact_mib <= 0 or args.max_member_mib <= 0:
        raise SystemExit("download limits must be positive")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    canonical_dir = args.output_dir / "canonical"
    artifact_re = re.compile(args.artifact_name_regex, re.IGNORECASE)
    client = GitHubArtifacts(args.repository, args.token, args.api_url)
    all_artifacts = client.list_all()
    selected = sorted(
        (
            artifact
            for artifact in all_artifacts
            if not artifact.get("expired")
            and artifact_re.search(str(artifact.get("name") or ""))
        ),
        key=artifact_priority,
    )

    candidates: list[Candidate] = []
    errors: list[dict[str, Any]] = []
    exact_candidate: Candidate | None = None
    exact_data: bytes | None = None
    downloaded = 0
    max_artifact_bytes = args.max_artifact_mib * 1024 * 1024
    max_member_bytes = args.max_member_mib * 1024 * 1024

    for artifact in selected:
        artifact_id = int(artifact.get("id") or 0)
        artifact_name = str(artifact.get("name") or f"artifact-{artifact_id}")
        try:
            archive = client.download(artifact, max_artifact_bytes)
            downloaded += 1
            found, canonical = scan_artifact_zip(
                archive,
                artifact_id=artifact_id,
                artifact_name=artifact_name,
                canonical_sha256=args.canonical_sha256,
                max_member_bytes=max_member_bytes,
            )
            candidates.extend(found)
            if canonical is not None:
                exact_data = canonical
                exact_candidate = next(item for item in found if item.exact_match)
                break
        except (
            OSError,
            RuntimeError,
            ValueError,
            zipfile.BadZipFile,
            urllib.error.URLError,
        ) as exc:
            errors.append(
                {
                    "artifact_id": artifact_id,
                    "artifact_name": artifact_name,
                    "error": str(exc),
                }
            )

    candidates.sort(
        key=lambda item: (item.artifact_name.lower(), item.member.lower(), item.sha256)
    )
    result: dict[str, Any] = {
        "schema": 2,
        "repository": args.repository,
        "canonical_sha256": args.canonical_sha256.lower(),
        "artifacts_seen": len(all_artifacts),
        "artifacts_selected": len(selected),
        "artifacts_downloaded": downloaded,
        "candidates": [candidate.as_dict() for candidate in candidates],
        "exact_match": exact_candidate.as_dict() if exact_candidate else None,
        "errors": errors,
    }

    if exact_data is not None:
        canonical_dir.mkdir(parents=True, exist_ok=True)
        canonical_path = canonical_dir / "Flash.fd"
        canonical_path.write_bytes(exact_data)
        (args.output_dir / "canonical-path.txt").write_text(
            str(canonical_path.resolve()) + "\n", encoding="utf-8"
        )

    (args.output_dir / "inventory.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (args.output_dir / "report.md").write_text(
        markdown_report(result), encoding="utf-8"
    )
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
