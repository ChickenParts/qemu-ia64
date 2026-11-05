#!/usr/bin/env bash
# Fetch the historical Xen/KVM IA-64 EFI virtual firmware without checking the
# third-party binary into this repository.
#
# The original IA-64 KVM instructions point at Xen's efi-vfirmware Mercurial
# repository and specifically its binaries directory.  This helper preserves
# that provenance and emits a checksum alongside the selected Flash.fd.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/fetch-ia64-xen-firmware.sh [OUTPUT_DIR]

Environment overrides:
  IA64_XEN_FIRMWARE_HG_URL   Mercurial repository URL
  IA64_XEN_FIRMWARE_REV      Mercurial revision (default: tip)
  IA64_XEN_FIRMWARE_KEEP_SRC Keep fetched source tree when nonzero

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
hg_rev="${IA64_XEN_FIRMWARE_REV:-tip}"
keep_src="${IA64_XEN_FIRMWARE_KEEP_SRC:-0}"

if ! command -v hg >/dev/null 2>&1; then
    echo "error: Mercurial (hg) is required" >&2
    exit 2
fi

mkdir -p "$out_dir"
out_dir="$(cd "$out_dir" && pwd)"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/ia64-efi-vfirmware.XXXXXX")"
cleanup() {
    if [[ "$keep_src" != "0" ]]; then
        rm -rf "$out_dir/source"
        mv "$work_dir/repo" "$out_dir/source"
    fi
    rm -rf "$work_dir"
}
trap cleanup EXIT

urls=(
    "$hg_url"
    "https://xenbits.xensource.com/ext/efi-vfirmware.hg"
    "http://xenbits.xensource.com/ext/efi-vfirmware.hg"
)

cloned_url=""
for url in "${urls[@]}"; do
    [[ -n "$url" ]] || continue
    rm -rf "$work_dir/repo"
    echo "Trying Xen IA-64 virtual firmware source: $url" >&2
    if hg clone -q -u "$hg_rev" "$url" "$work_dir/repo"; then
        cloned_url="$url"
        break
    fi
done

if [[ -z "$cloned_url" ]]; then
    echo "error: unable to clone the historical efi-vfirmware repository" >&2
    exit 1
fi

repo="$work_dir/repo"
revision="$(hg -R "$repo" id -i -r . | tr -d '+')"

candidate=""
for name in Flash.fd flash.fd GFW.fd gfw.fd; do
    found="$(find "$repo" -type f -path '*/binaries/*' -iname "$name" \
              -size +0c -print -quit)"
    if [[ -n "$found" ]]; then
        candidate="$found"
        break
    fi
done

if [[ -z "$candidate" ]]; then
    candidate="$(find "$repo" -type f -path '*/binaries/*' \
                  \( -iname '*.fd' -o -iname '*.rom' -o -iname '*.bin' \) \
                  -size +0c -printf '%s\t%p\n' | sort -nr | head -n 1 | \
                  cut -f2-)"
fi

if [[ -z "$candidate" || ! -s "$candidate" ]]; then
    echo "error: no nonempty IA-64 firmware binary found under binaries/" >&2
    find "$repo" -maxdepth 3 -type f -printf '%s\t%p\n' | sort -n >&2 || true
    exit 1
fi

cp "$candidate" "$out_dir/Flash.fd"
sha256="$(sha256sum "$out_dir/Flash.fd" | awk '{print $1}')"
size="$(stat -c '%s' "$out_dir/Flash.fd")"
source_rel="${candidate#"$repo"/}"

cat > "$out_dir/PROVENANCE.txt" <<EOF
source_url=$cloned_url
source_revision=$revision
source_path=$source_rel
firmware_size=$size
firmware_sha256=$sha256
fetched_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

cat "$out_dir/PROVENANCE.txt"
