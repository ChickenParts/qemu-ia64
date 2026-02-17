#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/ia64-pei-copy-report.sh [options] [logfile...]

Summarize IA64 PEI copy-path probe output (`IA64: fw_pei_copy ...`) and flag
pointer-like setup-count values.

Options:
  --ptr-min HEX_OR_DEC   Suspicious count threshold (default: 0x100000)
  --top N                Max suspicious samples to print (default: 12)
  -h, --help             Show help

If no logfile is provided, the script uses:
  scratch/ia64_logs/qemu.fw.log

Pointer-like means either:
  - canonical firmware pointer prefix (0xffffffff........ / 0x7fffffff........)
  - or value >= --ptr-min

Note:
  Suspicious count detection is based on `copy_setup` (`copy_enter` in older
  logs), because later loop bundles can transiently hold `-1` on normal
  zero-count exits.
EOF
}

ptr_min="0x100000"
top_n=12

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ptr-min)
      [[ $# -ge 2 ]] || { echo "error: --ptr-min requires a value" >&2; exit 2; }
      ptr_min="$2"
      shift 2
      ;;
    --top)
      [[ $# -ge 2 ]] || { echo "error: --top requires a value" >&2; exit 2; }
      top_n="$2"
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
  [[ -f scratch/ia64_logs/qemu.fw.log ]] && inputs+=(scratch/ia64_logs/qemu.fw.log)
fi

if [[ ${#inputs[@]} -eq 0 ]]; then
  echo "error: no input logs found" >&2
  exit 1
fi

awk -v ptr_min_in="$ptr_min" -v top_n="$top_n" '
function normhex(raw, s) {
  s = tolower(raw);
  sub(/^0x/, "", s);
  gsub(/[^0-9a-f]/, "", s);
  sub(/^0+/, "", s);
  return (s == "" ? "0" : s);
}

function hex_ge(a, b, la, lb) {
  a = normhex(a);
  b = normhex(b);
  la = length(a);
  lb = length(b);
  if (la > lb) {
    return 1;
  }
  if (la < lb) {
    return 0;
  }
  return (a >= b);
}

function is_pointer_like(hexv, hi) {
  hexv = normhex(hexv);
  hi = substr(sprintf("%016s", hexv), 1, 8);
  gsub(/ /, "0", hi);
  if (hi == "ffffffff" || hi == "7fffffff") {
    return 1;
  }
  return hex_ge(hexv, ptr_min);
}

function observe_counter(hexv, where, sample_key) {
  hexv = normhex(hexv);
  total_seen++;
  if (min_hex == "" || hex_ge(min_hex, hexv)) {
    min_hex = hexv;
  }
  if (max_hex == "" || hex_ge(hexv, max_hex)) {
    max_hex = hexv;
  }
  if (is_pointer_like(hexv)) {
    suspicious_seen++;
    if (suspicious_printed < top_n) {
      suspicious_printed++;
      suspicious[suspicious_printed] = where " value=0x" hexv;
    }
    sample_key = where "\t" hexv;
    suspicious_unique[sample_key] = 1;
  }
}

BEGIN {
  ptr_min = normhex(ptr_min_in);
  stage_cnt["copy_setup"] = 0;
  stage_cnt["copy_count_check"] = 0;
  stage_cnt["copy_count_dec"] = 0;
  stage_cnt["copy_body"] = 0;
  stage_cnt["copy_ptr_advance"] = 0;
  stage_cnt["copy_post"] = 0;
  stage_cnt["hob_store_pre"] = 0;
  stage_cnt["hob_store_post"] = 0;
}

function canon_stage(stage) {
  if (stage == "copy_enter") {
    return "copy_setup";
  }
  if (stage == "copy_loop") {
    return "copy_body";
  }
  if (stage == "copy_dec") {
    return "copy_ptr_advance";
  }
  if (stage == "copy_exit") {
    return "copy_post";
  }
  return stage;
}

{
  if (match($0, /IA64: fw_pei_copy ([a-z_]+) pc=/, m)) {
    stage = canon_stage(m[1]);
    current_stage = stage;
    if (!(stage in stage_cnt)) {
      stage_cnt[stage] = 0;
    }
    stage_cnt[stage]++;
  }
  if (current_stage == "copy_setup" &&
      match($0, /IA64: fw_pei_copy slot\+0x08 .* val=([0-9a-fA-F]+)/, m2)) {
    observe_counter(m2[1], "slot+0x08 @" FILENAME ":" FNR);
  }
  if (match($0, /IA64: fw_pei_copy_writer HIT pc=([0-9a-fA-F]+).*store_addr=([0-9a-fA-F]+).* val=([0-9a-fA-F]+)/, w)) {
    writer_hits++;
    if (writer_printed < top_n) {
      writer_printed++;
      writer_samples[writer_printed] = "pc=0x" normhex(w[1]) " addr=0x" normhex(w[2]) " val=0x" normhex(w[3]);
    }
  }
  if ($0 ~ /IA64: fw_pei_copy trigger /) {
    trigger_hits++;
  }
}

END {
  if (total_seen == 0) {
    print "No IA64 fw_pei_copy lines found in inputs.";
    exit 1;
  }

  print "PEI copy-path summary";
  print "  inputs: " ARGC - 1 " file(s)";
  print "  ptr_min: 0x" ptr_min;
  print "  stage counts:";
  print "    hob_store_pre:  " stage_cnt["hob_store_pre"];
  print "    hob_store_post: " stage_cnt["hob_store_post"];
  print "    copy_setup:     " stage_cnt["copy_setup"];
  print "    copy_count_check: " stage_cnt["copy_count_check"];
  print "    copy_count_dec: " stage_cnt["copy_count_dec"];
  print "    copy_body:      " stage_cnt["copy_body"];
  print "    copy_ptr_advance: " stage_cnt["copy_ptr_advance"];
  print "    copy_post:      " stage_cnt["copy_post"];
  print "  counter observations: " total_seen;
  print "  counter min/max: 0x" min_hex " .. 0x" max_hex;
  print "  suspicious observations: " suspicious_seen;
  print "  trigger hits: " trigger_hits;
  print "  writer hits: " writer_hits;

  if (suspicious_seen > 0) {
    print "  suspicious samples:";
    for (i = 1; i <= suspicious_printed; i++) {
      print "    - " suspicious[i];
    }
  }
  if (writer_hits > 0) {
    print "  writer samples:";
    for (i = 1; i <= writer_printed; i++) {
      print "    - " writer_samples[i];
    }
  }
}
' "${inputs[@]}"
