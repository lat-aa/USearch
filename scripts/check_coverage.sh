#!/usr/bin/env bash
# Fail if line coverage is below MIN_LINE_PCT (default 40 for include/usearch extract).
set -euo pipefail
INFO="${1:?coverage.info}"
MIN_LINE_PCT="${MIN_LINE_PCT:-40}"

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
  if (p+0 < m+0) { printf("coverage %.2f%% < min %s%%\n", p, m) > "/dev/stderr"; exit 1 }
  printf("coverage ok: %.2f%% >= %s%%\n", p, m)
}'
