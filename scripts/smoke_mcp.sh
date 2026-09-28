#!/usr/bin/env bash
# L0：MCP 协议 / Auth / SSE / tools/list（含 Codex 别名）。
# Usage: API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_mcp.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=scripts/smoke_common.sh
source "$ROOT/scripts/smoke_common.sh"

trap 'smoke_dump_log; smoke_teardown' ERR
smoke_boot
smoke_auth_headers
trap 'smoke_teardown' EXIT

TMP="${TMPDIR:-/tmp}/smoke_mcp_$$"
mkdir -p "$TMP"

# --- Auth on /mcp ---
code=$(curl -sS -o "$TMP/noauth.txt" -w '%{http_code}' --max-time 8 \
  -H 'Content-Type: application/json' -H 'Accept: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"ping"}' "$SMOKE_BASE/mcp" || true)
[[ "$code" = "401" ]] && smoke_pass "mcp auth deny no bearer" || smoke_bad "mcp auth deny" "code=$code"

code=$(curl -sS -o "$TMP/badauth.txt" -w '%{http_code}' --max-time 8 \
  -H 'Authorization: Bearer wrong' -H 'Content-Type: application/json' -H 'Accept: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"ping"}' "$SMOKE_BASE/mcp" || true)
[[ "$code" = "401" ]] && smoke_pass "mcp auth deny bad token" || smoke_bad "mcp auth deny bad" "code=$code"

# --- initialize → initialized 202 → tools/list ---
init=$(curl -fsS --max-time 15 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"smoke-mcp","version":"0"}}}' \
  "$SMOKE_BASE/mcp")
echo "$init" | python3 -c "import sys,json;assert json.load(sys.stdin)['result']['protocolVersion']" \
  && smoke_pass "mcp initialize" || smoke_bad "mcp initialize" "$init"

code=$(curl -sS -o "$TMP/inited.txt" -w '%{http_code}' --max-time 10 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}' "$SMOKE_BASE/mcp" || true)
[[ "$code" = "202" ]] && smoke_pass "mcp initialized 202" || smoke_bad "mcp initialized" "code=$code"

# 无 id 的其它 notification 亦为 202
code=$(curl -sS -o "$TMP/notif.txt" -w '%{http_code}' --max-time 10 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","method":"notifications/cancelled","params":{}}' "$SMOKE_BASE/mcp" || true)
[[ "$code" = "202" ]] && smoke_pass "mcp notification 202" || smoke_bad "mcp notification" "code=$code"

ping=$(curl -fsS --max-time 10 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":2,"method":"ping"}' "$SMOKE_BASE/mcp")
echo "$ping" | python3 -c "import sys,json;d=json.load(sys.stdin);assert 'result' in d or d.get('error',{}).get('code')!=-32601" \
  && smoke_pass "mcp ping" || smoke_bad "mcp ping" "$ping"

unk=$(curl -fsS --max-time 10 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":3,"method":"no/such/method"}' "$SMOKE_BASE/mcp")
echo "$unk" | python3 -c "import sys,json;assert json.load(sys.stdin)['error']['code']==-32601" \
  && smoke_pass "mcp unknown method -32601" || smoke_bad "mcp unknown" "$unk"

badj=$(curl -fsS --max-time 10 "${MCP[@]}" \
  -d 'not-json{' "$SMOKE_BASE/mcp" || true)
echo "$badj" | python3 -c "import sys,json;d=json.load(sys.stdin);assert d['error']['code']==-32700" \
  && smoke_pass "mcp bad json -32700" || smoke_bad "mcp bad json" "$badj"

tlist=$(curl -fsS --max-time 15 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools/list","params":{}}' "$SMOKE_BASE/mcp")
echo "$tlist" | python3 -c "
import sys,json
names={t['name'] for t in json.load(sys.stdin)['result']['tools']}
need={'rules','catalog','rule','search','upsert','delete','recall','decide','observe','cost',
      'resolve-rules','list-rules','get-rule','shell','test','status','commit','save'}
miss=need-names
assert not miss, miss
assert 'gate' not in names
assert len(names)>=18
" && smoke_pass "mcp tools/list full+aliases" || smoke_bad "mcp tools/list" "missing"

# --- GET SSE：仅 comment，禁止 data: { ---
curl -fsS --max-time 5 -H "Accept: text/event-stream" "${AUTH[@]}" \
  "$SMOKE_BASE/mcp" >"$TMP/get_sse.txt" || true
if grep -q '^data: {' "$TMP/get_sse.txt" 2>/dev/null; then
  smoke_bad "mcp GET SSE" "leaked data: {"
else
  smoke_pass "mcp GET SSE comment-only"
fi

# --- POST SSE：Accept 仅 text/event-stream 时包 event: message ---
sse=$(curl -fsS --max-time 15 \
  -H "Authorization: Bearer ${SMOKE_TOKEN}" \
  -H "Content-Type: application/json" \
  -H "Accept: text/event-stream" \
  -d '{"jsonrpc":"2.0","id":5,"method":"ping"}' \
  "$SMOKE_BASE/mcp")
echo "$sse" | python3 -c "
import sys
t=sys.stdin.read()
assert 'event: message' in t, t[:200]
assert 'data: {' in t, t[:200]
" && smoke_pass "mcp POST SSE event:message" || smoke_bad "mcp POST SSE" "$(echo "$sse"|head -c200)"

smoke_summary
echo OK
