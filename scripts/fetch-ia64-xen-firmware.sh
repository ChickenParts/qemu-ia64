#!/usr/bin/env bash
# Fetch the historical Xen/KVM IA-64 EFI virtual firmware without checking the
# third-party binary into this repository.
#
# This repository contains old EDK2 trees whose working-copy update path has
# host-build side effects.  We only need one already-built firmware blob, so
# clone without a working copy and extract that blob directly from Mercurial.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/fetch-ia64-xen-firmware.sh [OUTPUT_DIR]

Environment overrides:
  IA64_XEN_FIRMWARE_HG_URL   Preferred Mercurial repository URL
  IA64_XEN_FIRMWARE_REV      Mercurial revision
  IA64_XEN_FIRMWARE_PATH     Preferred repository path to Flash.fd
  IA64_XEN_FIRMWARE_SHA256   Required firmware digest
  IA64_XEN_FIRMWARE_KEEP_SRC Keep the no-working-copy repository when nonzero

The selected firmware is written to OUTPUT_DIR/Flash.fd and its provenance to
OUTPUT_DIR/PROVENANCE.txt.  Third-party license terms remain applicable.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

out_dir="${1:-scratch/ia64-xen-firmware}"
hg_url="${IA64_XEN_FIRMWARE_HG_URL:-https://xenbits.xen.org/ext/efi-vfirmware.hg}"
hg_rev="${IA64_XEN_FIRMWARE_REV:-633569c9b875}"
preferred_path="${IA64_XEN_FIRMWARE_PATH:-ovmf-ia64-r17699-xen-ia64/Flash.fd}"
expected_sha256="${IA64_XEN_FIRMWARE_SHA256:-e143e85874ad57bad631853d48f0d47b7e7dbe6c41b4e558bbb4ea5b45775513}"
keep_src="${IA64_XEN_FIRMWARE_KEEP_SRC:-0}"

if ! command -v hg >/dev/null 2>&1; then
    echo "error: Mercurial (hg) is required" >&2
    exit 2
fi

mkdir -p "$out_dir"
out_dir="$(cd "$out_dir" && pwd)"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/ia64-efi-vfirmware.XXXXXX")"
repo="$work_dir/repo"
cleanup() {
    if [[ "$keep_src" != "0" && -d "$repo/.hg" ]]; then
        rm -rf "$out_dir/source"
        mv "$repo" "$out_dir/source"
    fi
    rm -rf "$work_dir"
}
trap cleanup EXIT

urls=(
    "$hg_url"
    "https://xenbits.xen.org/ext/efi-vfirmware.hg"
    "https://xenbits.xen.org/git-http/people/awilliam/efi-vfirmware.hg"
    "https://xenbits.xensource.com/ext/efi-vfirmware.hg"
)

cloned_url=""
for url in "${urls[@]}"; do
    [[ -n "$url" ]] || continue
    rm -rf "$repo"
    echo "Trying Xen IA-64 virtual firmware source: $url" >&2
    if HGPLAIN=1 hg clone -q -U -r "$hg_rev" "$url" "$repo"; then
        cloned_url="$url"
        break
    fi
done

if [[ -z "$cloned_url" ]]; then
    echo "error: unable to clone revision $hg_rev without a working copy" >&2
    exit 1
fi

revision="$(HGPLAIN=1 hg -R "$repo" log -r "$hg_rev" --template '{node}\n')"
if [[ -z "$revision" ]]; then
    echo "error: cloned repository did not resolve revision $hg_rev" >&2
    exit 1
fi

manifest="$work_dir/manifest.txt"
HGPLAIN=1 hg -R "$repo" manifest -r "$revision" > "$manifest"

# Prefer the historically identified image, but tolerate a containing
# binaries/ directory and then search only IA-64 firmware-shaped paths.  Every
# candidate still has to match the pinned digest before it can be installed.
candidates="$work_dir/candidates.txt"
{
    printf '%s\n' "$preferred_path"
    printf 'binaries/%s\n' "$preferred_path"
    grep -Ei '(^|/)(binaries/)?[^/]*ia64[^/]*/(Flash|GFW)\.fd$' "$manifest" || true
    grep -Ei '(^|/)binaries/.*\.(fd|rom|bin)$' "$manifest" || true
} | awk 'NF && !seen[$0]++' | while IFS= read -r path; do
    grep -Fqx -- "$path" "$manifest" && printf '%s\n' "$path"
done > "$candidates"

if [[ ! -s "$candidates" ]]; then
    echo "error: no IA-64 firmware candidates exist at revision $revision" >&2
    exit 1
fi

selected_path=""
selected_file="$work_dir/Flash.fd"
while IFS= read -r path; do
    candidate="$work_dir/candidate.fd"
    HGPLAIN=1 hg -R "$repo" cat -r "$revision" "$path" > "$candidate"
    [[ -s "$candidate" ]] || continue
    digest="$(sha256sum "$candidate" | awk '{print $1}')"
    if [[ "$digest" == "$expected_sha256" ]]; then
        mv "$candidate" "$selected_file"
        selected_path="$path"
        break
    fi
    printf 'Rejected %s: sha256=%s\n' "$path" "$digest" >&2
done < "$candidates"

if [[ -z "$selected_path" ]]; then
    echo "error: no candidate matched required sha256 $expected_sha256" >&2
    exit 1
fi

install -m 0644 "$selected_file" "$out_dir/Flash.fd"
sha256="$(sha256sum "$out_dir/Flash.fd" | awk '{print $1}')"
size="$(stat -c '%s' "$out_dir/Flash.fd")"

provenance_tmp="$work_dir/PROVENANCE.txt"
cat > "$provenance_tmp" <<EOF
source_url=$cloned_url
source_revision=$revision
source_path=$selected_path
acquisition=hg-cat-no-working-copy
firmware_size=$size
firmware_sha256=$sha256
fetched_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF
install -m 0644 "$provenance_tmp" "$out_dir/PROVENANCE.txt"

cat "$out_dir/PROVENANCE.txt"
