#!/usr/bin/env bash
# L4 nightly：observe Worker、chat/responses 流式长生成（前置 gate 已移除）。
# Usage: API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_nightly.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# 避免与默认 8088 上其它 smoke 抢端口（须在 source 前设定）
export PORT="${PORT:-18088}"
export HOST="${HOST:-127.0.0.1}"
# shellcheck source=scripts/smoke_common.sh
source "$ROOT/scripts/smoke_common.sh"

trap 'smoke_dump_log; smoke_teardown' ERR
smoke_boot
smoke_auth_headers
trap 'smoke_teardown' EXIT

smoke_skip "nightly gate conflict" "gate plane removed"
smoke_skip "nightly gate L1" "gate plane removed"
smoke_skip "nightly chat short-circuit" "gate plane removed"

# --- observe → Worker（异步；有 GGUF 时等待 mem-*）---
obs=$(curl -fsS --max-time 30 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"observe","arguments":{"title":"nightly","summary":"distill worker smoke about naming flags","outcome":"ok","tags":["nightly"]}}}' \
  "$SMOKE_BASE/mcp")
echo "$obs" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert t.get('ok') is True and t.get('status')=='pending'
" && smoke_pass "nightly observe enqueue" || smoke_bad "nightly observe" "fail"

if [[ "$SMOKE_HAS_GGUF" -eq 1 ]]; then
  sleep 8
  sr=$(curl -fsS --max-time 60 "${MCP[@]}" \
    -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"search","arguments":{"text":"naming flags distill","k":8}}}' \
    "$SMOKE_BASE/mcp")
  echo "$sr" | python3 -c "
import sys,json
arr=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
print('hits', len(arr) if isinstance(arr,list) else 0)
" && smoke_pass "nightly search after observe" || smoke_bad "nightly search" "fail"

  curl -fsS --max-time 120 "${AUTH[@]}" \
    -H "Content-Type: application/json" -H "Accept: text/event-stream" \
    -d '{"model":"deepseek-flash","stream":true,"messages":[{"role":"user","content":"say hi"}],"max_tokens":64}' \
    "$SMOKE_BASE/v1/chat/completions" >"${TMPDIR:-/tmp}/nightly_chat_sse.txt"
  python3 -c "
t=open('${TMPDIR:-/tmp}/nightly_chat_sse.txt',encoding='utf-8',errors='replace').read()
assert 'chat.completion.chunk' in t and 'data: [DONE]' in t
assert t.count('delta')>=1
" && smoke_pass "nightly chat stream long" || smoke_bad "nightly stream chat" "fail"

  curl -fsS --max-time 120 "${AUTH[@]}" \
    -H "Content-Type: application/json" -H "Accept: text/event-stream" \
    -d '{"model":"deepseek-v4-pro","stream":true,"input":"say hi","max_output_tokens":64}' \
    "$SMOKE_BASE/v1/responses" >"${TMPDIR:-/tmp}/nightly_resp_sse.txt"
  python3 -c "
t=open('${TMPDIR:-/tmp}/nightly_resp_sse.txt',encoding='utf-8',errors='replace').read()
assert 'event: response.created' in t
assert 'event: response.completed' in t
assert 'event: response.output_text.delta' in t or 'output_text' in t
" && smoke_pass "nightly responses stream long" || smoke_bad "nightly stream resp" "fail"
else
  smoke_skip "nightly Worker distill mem-*" "SKIP_NO_GGUF"
  smoke_skip "nightly chat/responses stream" "SKIP_NO_GGUF"
fi

echo "== nested mcp auth =="
code=$(curl -sS -o /dev/null -w '%{http_code}' --max-time 8 \
  -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"ping"}' "$SMOKE_BASE/mcp" || true)
[[ "$code" = "401" ]] && smoke_pass "nightly nested mcp auth" || smoke_bad "nightly nested auth" "code=$code"

smoke_summary
echo OK
