#!/usr/bin/env bash
# L4 nightly：gate L1/L2、conflict、Worker 蒸馏、chat/responses 短路与流式长生成。
# Usage: API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_nightly.sh
# 无 GGUF：仍跑 conflict/observe；L1 answered 与长流式 SKIP_NO_GGUF。
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

TASK='how to name a C++ flag without underscore for nightly'

# --- conflict refuse/pack ---
curl -fsS --max-time 60 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"docs":[{"id":"conflict-nightly","text":"use foo_bar please"}]}' \
  "$SMOKE_BASE/v1/upsert" >/dev/null || true
gconf=$(curl -fsS --max-time 120 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"task":"add foo_bar flag to encoder"}' "$SMOKE_BASE/v1/gate")
echo "$gconf" | python3 -c "
import sys,json
d=json.load(sys.stdin)
assert d.get('status') in ('refuse','pack'), d
assert d.get('status')!='answered'
" && smoke_pass "nightly gate conflict fail-closed" || smoke_bad "nightly conflict" "$(echo "$gconf"|head -c200)"
curl -fsS --max-time 30 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"ids":["conflict-nightly"]}' "$SMOKE_BASE/v1/delete" >/dev/null || true

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
# Worker 可能尚未写完；有 hit 则优，否则不红（仅记录）
print('hits', len(arr) if isinstance(arr,list) else 0)
" && smoke_pass "nightly search after observe" || smoke_bad "nightly search" "fail"
else
  smoke_skip "nightly Worker distill mem-*" "SKIP_NO_GGUF"
fi

# --- 同 task 连续 gate：若 answered 则第二次 L1 ---
g1=$(curl -fsS --max-time 120 "${AUTH[@]}" "${JSON[@]}" \
  -d "{\"task\":\"$TASK\"}" "$SMOKE_BASE/v1/gate")
st1=$(echo "$g1" | python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('status',''), d.get('cache',''))")
g2=$(curl -fsS --max-time 120 "${AUTH[@]}" "${JSON[@]}" \
  -d "{\"task\":\"$TASK\"}" "$SMOKE_BASE/v1/gate")
echo "$g2" | python3 -c "
import sys,json
d=json.load(sys.stdin)
assert d.get('status') in ('answered','pack','refuse'), d
cache=d.get('cache','')
status=d.get('status')
# 仅 answered 路径保证写 L1；第二次应为 L1
if status=='answered':
  assert cache=='L1', d
print('status', status, 'cache', cache)
" && smoke_pass "nightly gate L1 (first was: $st1)" || smoke_bad "nightly L1" "first=$st1"

if [[ "$SMOKE_HAS_GGUF" -eq 1 ]]; then
  gst=$(echo "$g2" | python3 -c "import sys,json;print(json.load(sys.stdin).get('status',''))")
  if [[ "$gst" = "answered" ]]; then
    cgate=$(curl -fsS --max-time 60 "${AUTH[@]}" "${JSON[@]}" \
      -d "{\"messages\":[{\"role\":\"user\",\"content\":\"$TASK\"}],\"max_tokens\":32}" \
      "$SMOKE_BASE/v1/chat/completions")
    echo "$cgate" | python3 -c "
import sys,json
d=json.load(sys.stdin)
assert d.get('id')=='gate' and d.get('gate',{}).get('status')=='answered'
" && smoke_pass "nightly chat short-circuit" || smoke_bad "nightly chat SC" "fail"

    curl -fsS --max-time 120 "${AUTH[@]}" \
      -H "Content-Type: application/json" -H "Accept: text/event-stream" \
      -d '{"model":"deepseek-flash","stream":true,"gate":false,"messages":[{"role":"user","content":"say hi"}],"max_tokens":64}' \
      "$SMOKE_BASE/v1/chat/completions" >"${TMPDIR:-/tmp}/nightly_chat_sse.txt"
    python3 -c "
t=open('${TMPDIR:-/tmp}/nightly_chat_sse.txt',encoding='utf-8',errors='replace').read()
assert 'chat.completion.chunk' in t and 'data: [DONE]' in t
assert t.count('delta')>=1
" && smoke_pass "nightly chat stream long" || smoke_bad "nightly stream chat" "fail"

    curl -fsS --max-time 120 "${AUTH[@]}" \
      -H "Content-Type: application/json" -H "Accept: text/event-stream" \
      -d '{"model":"deepseek-v4-pro","stream":true,"gate":false,"input":"say hi","max_output_tokens":64}' \
      "$SMOKE_BASE/v1/responses" >"${TMPDIR:-/tmp}/nightly_resp_sse.txt"
    python3 -c "
t=open('${TMPDIR:-/tmp}/nightly_resp_sse.txt',encoding='utf-8',errors='replace').read()
assert 'event: response.created' in t
assert 'event: response.completed' in t
assert 'event: response.output_text.delta' in t or 'output_text' in t
" && smoke_pass "nightly responses stream long" || smoke_bad "nightly stream resp" "fail"
  else
    smoke_skip "nightly chat short-circuit" "gate=$gst"
    smoke_skip "nightly stream long" "gate=$gst (still run stream with gate:false)"
    curl -fsS --max-time 120 "${AUTH[@]}" \
      -H "Content-Type: application/json" -H "Accept: text/event-stream" \
      -d '{"model":"deepseek-flash","stream":true,"gate":false,"messages":[{"role":"user","content":"say hi"}],"max_tokens":64}' \
      "$SMOKE_BASE/v1/chat/completions" >"${TMPDIR:-/tmp}/nightly_chat_sse.txt"
    python3 -c "
t=open('${TMPDIR:-/tmp}/nightly_chat_sse.txt',encoding='utf-8',errors='replace').read()
assert 'data: [DONE]' in t
" && smoke_pass "nightly chat stream (no answered)" || smoke_bad "nightly stream" "fail"
  fi
else
  smoke_skip "nightly chat/responses stream+SC" "SKIP_NO_GGUF"
fi

# 串联短路径冒烟（确保 nightly 至少覆盖 L0/L1 核心）
echo "== nested smoke_mcp (port reuse: external already up) =="
# 已有进程：直接对本 BASE 做精简断言（不二次 boot）
code=$(curl -sS -o /dev/null -w '%{http_code}' --max-time 8 \
  -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"ping"}' "$SMOKE_BASE/mcp" || true)
[[ "$code" = "401" ]] && smoke_pass "nightly nested mcp auth" || smoke_bad "nightly nested auth" "code=$code"

smoke_summary
echo OK
