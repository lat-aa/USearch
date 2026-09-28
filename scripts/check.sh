#!/usr/bin/env bash
# 本地质量门禁（make check / make lint / make test / make check-e2e）。
# 用法：./scripts/check.sh [lint|unit|e2e|all]  默认 all = lint + unit
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-all}"

say() { printf '\n==> %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

do_lint() {
  say "bash -n"
  local f
  for f in "$ROOT"/scripts/*.sh; do bash -n "$f" || fail "bash -n $f"; done

  say "shellcheck"
  if have shellcheck; then
    shellcheck -S warning "$ROOT"/scripts/*.sh || fail "shellcheck"
  else
    echo "SKIP shellcheck (not installed)"
  fi

  say "node --check"
  if have node; then
    for f in "$ROOT"/scripts/*.js; do node --check "$f" || fail "node --check $f"; done
  else
    echo "SKIP node --check (not installed)"
  fi

  say "clang-format --dry-run --Werror"
  if have clang-format; then
    mapfile -t files < <(find "$ROOT/tools/apex" "$ROOT/tools/sqlite" \( -name '*.hpp' -o -name '*.cpp' \) | sort)
    clang-format --dry-run --Werror "${files[@]}" || fail "clang-format"
  else
    echo "SKIP clang-format (not installed)"
  fi
}

do_unit() {
  say "ctest -L apex"
  local build="${BUILD_DIR:-$ROOT/build}"
  [[ -d "$build" ]] || fail "no build dir: $build (set BUILD_DIR=...) or run cmake first"
  ctest --test-dir "$build" --output-on-failure -L apex || fail "ctest -L apex"
}

do_e2e() {
  say "e2e smoke (hash mode)"
  local api_bin="${API_BIN:-$ROOT/build/api}"
  [[ -x "$api_bin" || -f "$api_bin" ]] || fail "missing api binary: $api_bin (set API_BIN=...)"

  # shellcheck source=scripts/smoke_common.sh
  source "$ROOT/scripts/smoke_common.sh"
  export SMOKE_HASH=1
  smoke_boot
  APEX_BASE="$SMOKE_BASE" APEX_TOKEN="$SMOKE_TOKEN" node "$ROOT/scripts/smoke_hooks.js" || fail "smoke_hooks"
  smoke_teardown

  API_BIN="$api_bin" TOKEN=sk-default "$ROOT/scripts/smoke_agent.sh" || fail "smoke_agent"
  API_BIN="$api_bin" TOKEN=sk-default "$ROOT/scripts/smoke_mcp.sh" || fail "smoke_mcp"
  API_BIN="$api_bin" TOKEN=sk-default "$ROOT/scripts/smoke_v1.sh" || fail "smoke_v1"
}

case "$MODE" in
  lint) do_lint ;;
  unit) do_unit ;;
  e2e)  do_e2e ;;
  all)  do_lint; do_unit ;;
  *) echo "usage: $0 [lint|unit|e2e|all]" >&2; exit 2 ;;
esac

echo
echo "check ok: $MODE"
