#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/ia64-unimpl-report.sh [options] [logfile...]

Aggregate IA64 UNIMPL lines and rank by hit count.

Options:
  --top N               Show only the top N entries (default: all)
  --format FORMAT       Output format: markdown | tsv (default: markdown)
  -h, --help            Show this help

If no logfile is provided, the script uses existing defaults in this order:
  run.repro.err run.watch.err scratch/ia64_logs/qemu.fw.log scratch/ia64_logs/qemu.log

Recognized line format example:
  IA64 UNIMPL: pc=a00000010002e240 ri=2 insn=14000000202 A-slot
EOF
}

top_n=0
format="markdown"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --top)
      [[ $# -ge 2 ]] || { echo "error: --top requires a value" >&2; exit 2; }
      top_n="$2"
      shift 2
      ;;
    --format)
      [[ $# -ge 2 ]] || { echo "error: --format requires a value" >&2; exit 2; }
      format="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "error: unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      break
      ;;
  esac
done

case "$format" in
  markdown|tsv) ;;
  *)
    echo "error: unsupported format '$format' (expected markdown or tsv)" >&2
    exit 2
    ;;
esac

if ! [[ "$top_n" =~ ^[0-9]+$ ]]; then
  echo "error: --top must be a non-negative integer" >&2
  exit 2
fi

declare -a inputs=()
if [[ $# -gt 0 ]]; then
  for f in "$@"; do
    if [[ -f "$f" ]]; then
      inputs+=("$f")
    else
      echo "warning: skipping missing file: $f" >&2
    fi
  done
else
  for f in run.repro.err run.watch.err scratch/ia64_logs/qemu.fw.log scratch/ia64_logs/qemu.log; do
    [[ -f "$f" ]] && inputs+=("$f")
  done
fi

if [[ ${#inputs[@]} -eq 0 ]]; then
  echo "error: no input logs found" >&2
  exit 1
fi

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

awk '
  {
    if (match($0, /IA64 UNIMPL: pc=([0-9a-fA-F]+) ri=([0-9]+) insn=([0-9a-fA-F]+) ([A-Za-z-]+-slot)/, m)) {
      pc = tolower(m[1]);
      ri = m[2];
      insn = tolower(m[3]);
      slot = m[4];
      key = pc "\t" ri "\t" insn "\t" slot;
      cnt[key]++;
      if (!(key in first_seen)) {
        first_seen[key] = FILENAME ":" FNR;
      }
    }
  }
  END {
    for (k in cnt) {
      print cnt[k] "\t" k "\t" first_seen[k];
    }
  }
' "${inputs[@]}" | LC_ALL=C sort -t$'\t' -k1,1nr -k2,2 > "$tmp"

if [[ ! -s "$tmp" ]]; then
  echo "No IA64 UNIMPL lines found in input logs." >&2
  exit 1
fi

limit_cmd=(cat "$tmp")
if [[ "$top_n" -gt 0 ]]; then
  limit_cmd=(head -n "$top_n" "$tmp")
fi

if [[ "$format" == "tsv" ]]; then
  echo -e "count\tpc\tri\tinsn\tslot\tfirst_seen"
  "${limit_cmd[@]}"
  exit 0
fi

echo "| Count | PC | RI | Instruction | Slot | First Seen |"
echo "| ---: | --- | ---: | --- | --- | --- |"
"${limit_cmd[@]}" | while IFS=$'\t' read -r count pc ri insn slot first_seen; do
  printf '| %s | `%s` | %s | `%s` | `%s` | `%s` |\n' \
    "$count" "$pc" "$ri" "$insn" "$slot" "$first_seen"
done
