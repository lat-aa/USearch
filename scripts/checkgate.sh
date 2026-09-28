#!/usr/bin/env bash
# 校验 render.hpp 合并行覆盖 ≥ MIN_LINE_PCT（默认 80）。
# 用法：./scripts/checkgate.sh coverage_gate.info
set -euo pipefail
INFO="${1:?coverage.info}"
MIN_LINE_PCT="${MIN_LINE_PCT:-80}"

list="$(lcov --summary "$INFO" 2>&1 || true)"
echo "$list"
pct="$(echo "$list" | sed -n 's/.*lines[.:]* *\([0-9.]*\)%.*/\1/p' | head -1)"
if [[ -z "$pct" ]]; then
  pct="$(echo "$list" | awk '/lines\.*:/ { for(i=1;i<=NF;i++) if ($i ~ /%/) { gsub(/%/,"",$i); print $i; exit } }')"
fi
if [[ -z "$pct" ]]; then
  echo "could not parse line coverage from lcov summary" >&2
  exit 1
fi
awk -v p="$pct" -v m="$MIN_LINE_PCT" 'BEGIN {
  if (p+0 < m+0) { printf("gate coverage %.2f%% < min %s%%\n", p, m) > "/dev/stderr"; exit 1 }
  printf("gate coverage ok: %.2f%% >= %s%%\n", p, m)
}'
