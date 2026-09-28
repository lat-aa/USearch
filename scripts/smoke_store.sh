#!/usr/bin/env bash
# 记忆持久化回归（隔离库）：upsert → 杀进程 → 重启 → search 命中。
# 需进程控制；nightly/本地使用，不进 prerelease 快路径。
# 用法：API_BIN=/path/api bash scripts/smoke_store.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
API_BIN="${API_BIN:-$ROOT/build/api}"
HOST="127.0.0.1"
PORT="${PORT:-8093}"
BASE="http://${HOST}:${PORT}"

PASS=0; FAIL=0
pass() { echo "PASS  $1"; PASS=$((PASS+1)); }
bad()  { echo "FAIL  $1 — $2"; FAIL=$((FAIL+1)); }

[[ -x "$API_BIN" || -f "$API_BIN" ]] || { echo "missing api binary: $API_BIN" >&2; exit 1; }

TMP="${TMPDIR:-/tmp}/smoke_store_$$"
mkdir -p "$TMP"
DB="$TMP/store.sqlite"
IDX="$TMP/index.usearch"
LOG="$TMP/api.log"
PIDF="$TMP/api.pid"
CFG_BAK="$TMP/config.bak.toml"
rm -f "$DB" "$IDX"

# 备份并最终还原生产 config.toml：write_cfg 会覆盖它（隔离 base/index/gguf）。
[ -f "$ROOT/.config/config.toml" ] && cp "$ROOT/.config/config.toml" "$CFG_BAK"
restore_cfg() {
  if [ -f "$CFG_BAK" ]; then
    cp "$CFG_BAK" "$ROOT/.config/config.toml"
  fi
}

# 自建隔离配置；不依赖 smoke_boot 的「每次 boot 清库」隔离逻辑（持久化测试正需要不清库）。
write_cfg() {
  cp "$ROOT/.config/config.example.toml" "$ROOT/.config/config.toml"
  sed -i -E "s|^listen = .*|listen = \"${HOST}:${PORT}\"|" "$ROOT/.config/config.toml"
  sed -i -E "s|^token = .*|token = \"sk-default\"|" "$ROOT/.config/config.toml"
  sed -i -E "s|^base = .*|base = \"$DB\"|" "$ROOT/.config/config.toml"
  sed -i -E "s|^index = .*|index = \"$IDX\"|" "$ROOT/.config/config.toml"
  sed -i -E "s|^gguf = .*|gguf = \".config/models/MISSING-smoke.gguf\"|" "$ROOT/.config/config.toml"
}

start_api() {
  : >"$LOG"
  (cd "$ROOT" && "$API_BIN" serve) >>"$LOG" 2>&1 &
  echo $! >"$PIDF"
  for _ in $(seq 1 120); do
    if curl -fsS --max-time 2 -H "Authorization: Bearer sk-default" "$BASE/ready" 2>/dev/null \
      | python3 -c "import sys,json;sys.exit(0 if json.load(sys.stdin).get('ok') is True else 1)" 2>/dev/null; then
      return 0
    fi
    if ! kill -0 "$(cat "$PIDF")" 2>/dev/null; then
      echo "api exited before ready" >&2; cat "$LOG" >&2; exit 1
    fi
    sleep 1
  done
  echo "api not ready in 120s" >&2; cat "$LOG" >&2; exit 1
}

stop_api() {
  local pid
  pid="$(cat "$PIDF" 2>/dev/null || true)"
  [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
  sleep 1
  rm -f "$PIDF"
}

MCP=(-H "Authorization: Bearer sk-default" -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream")

UNIQ="smoke-store-persist-$$"
Q="persistence probe $UNIQ"

write_cfg
start_api

# S1: upsert 默认 kind=memory（真 bug 回归）
up=$(curl -fsS --max-time 15 "${MCP[@]}" \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"upsert\",\"arguments\":{\"id\":\"$UNIQ\",\"text\":\"$Q\"}}}" \
  "$BASE/mcp")
echo "$up" | python3 -c "import sys,json;t=json.load(sys.stdin)['result']['content'][0]['text'];assert t=='ok',t" \
  && pass "S1 upsert 默认 kind=memory" || bad "S1 upsert" "$(echo "$up" | head -c200)"

# S2: 在线可召回
rec=$(curl -fsS --max-time 15 "${MCP[@]}" \
  -d "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"search\",\"arguments\":{\"text\":\"$Q\",\"k\":5}}}" \
  "$BASE/mcp")
echo "$rec" | python3 -c "import sys,json;t=json.load(sys.stdin)['result']['content'][0]['text'];assert '$UNIQ' in t,t" \
  && pass "S2 在线 search 召回 memory" || bad "S2 search" "$(echo "$rec" | head -c200)"

# 杀进程 → 重启（不清库）
stop_api
write_cfg
start_api

# S3: 重启后仍召回
rec2=$(curl -fsS --max-time 15 "${MCP[@]}" \
  -d "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"search\",\"arguments\":{\"text\":\"$Q\",\"k\":5}}}" \
  "$BASE/mcp")
echo "$rec2" | python3 -c "import sys,json;t=json.load(sys.stdin)['result']['content'][0]['text'];assert '$UNIQ' in t,t" \
  && pass "S3 重启后 search 仍命中（持久化）" || bad "S3 persist" "$(echo "$rec2" | head -c200)"

stop_api
restore_cfg
echo
echo "======== SUMMARY pass=$PASS fail=$FAIL ========"
[[ "$FAIL" -eq 0 ]]
