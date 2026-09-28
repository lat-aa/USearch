#!/usr/bin/env bash
# L1：OpenAI 兼容 /v1 网关全量（models/embed/chat/responses/stream/route/rules/memory；gate 已移除）。
# Usage: API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_v1.sh
# 无 GGUF：V9–V14/V17/V19–V20 标记 SKIP_NO_GGUF（退出码仍 0）；短路径始终跑。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=scripts/smoke_common.sh
source "$ROOT/scripts/smoke_common.sh"

trap 'smoke_dump_log; smoke_teardown' ERR
smoke_boot
smoke_auth_headers
trap 'smoke_teardown' EXIT

TMP="${TMPDIR:-/tmp}/smoke_v1_$$"
mkdir -p "$TMP"

# ========== L1-A 鉴权 + models ==========
code=$(curl -sS -o "$TMP/uauth.txt" -w '%{http_code}' --max-time 8 "$SMOKE_BASE/v1/models" || true)
[[ "$code" = "401" ]] && smoke_pass "V0 auth deny no bearer" || smoke_bad "V0 auth deny" "code=$code"
code=$(curl -sS -o "$TMP/ubad.txt" -w '%{http_code}' --max-time 8 \
  -H "Authorization: Bearer wrong" "$SMOKE_BASE/v1/models" || true)
[[ "$code" = "401" ]] && smoke_pass "V0 auth deny bad token" || smoke_bad "V0 auth deny bad" "code=$code"

models=$(curl -fsS --max-time 10 "${AUTH[@]}" "$SMOKE_BASE/v1/models")
echo "$models" | python3 -c "
import sys,json
ids={x['id'] for x in json.load(sys.stdin)['data']}
assert 'deepseek-flash' in ids and 'deepseek-v4-pro' in ids
assert any('Nanbeige' in i for i in ids)
" && smoke_pass "V1 v1/models catalog" || smoke_bad "V1 models" "$models"

# ========== L1-B embed / memory ==========
emb=$(curl -fsS --max-time 60 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"input":"smoke-v1 embed"}' "$SMOKE_BASE/v1/embeddings")
echo "$emb" | python3 -c "import sys,json;e=json.load(sys.stdin)['data'][0]['embedding'];assert len(e)>8" \
  && smoke_pass "V2 v1/embeddings" || smoke_bad "V2 embeddings" "short"

emb2=$(curl -fsS --max-time 60 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"input":"smoke-v1 embed alias"}' "$SMOKE_BASE/v1/embed")
echo "$emb2" | python3 -c "import sys,json;e=json.load(sys.stdin)['data'][0]['embedding'];assert len(e)>8" \
  && smoke_pass "V3 v1/embed alias" || smoke_bad "V3 embed" "short"

DOC_ID="smoke-v1-$(date +%s)-$$"
up=$(curl -fsS --max-time 60 "${AUTH[@]}" "${JSON[@]}" \
  -d "{\"docs\":[{\"id\":\"$DOC_ID\",\"text\":\"smoke v1 auth vector document\"}]}" \
  "$SMOKE_BASE/v1/upsert")
echo "$up" | python3 -c "import sys,json;assert json.load(sys.stdin).get('upserted',0)>=1" \
  && smoke_pass "V4 upsert" || smoke_bad "V4 upsert" "$up"

sr=$(curl -fsS --max-time 60 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"text":"smoke v1 auth","k":5}' "$SMOKE_BASE/v1/search")
echo "$sr" | python3 -c "
import sys,json
hits=json.load(sys.stdin).get('hits') or []
assert any(h.get('id')=='''$DOC_ID''' for h in hits), hits
" && smoke_pass "V4 search hit" || smoke_bad "V4 search" "$(echo "$sr"|head -c200)"

dl=$(curl -fsS --max-time 30 "${AUTH[@]}" "${JSON[@]}" \
  -d "{\"ids\":[\"$DOC_ID\"]}" "$SMOKE_BASE/v1/delete")
echo "$dl" | python3 -c "import sys,json;assert json.load(sys.stdin).get('deleted',0)>=1" \
  && smoke_pass "V4 delete" || smoke_bad "V4 delete" "$dl"

sr2=$(curl -fsS --max-time 60 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"text":"smoke v1 auth","k":5}' "$SMOKE_BASE/v1/search")
echo "$sr2" | python3 -c "
import sys,json
hits=json.load(sys.stdin).get('hits') or []
assert not any(h.get('id')=='''$DOC_ID''' for h in hits), hits
" && smoke_pass "V4 search after delete" || smoke_bad "V4 search gone" "still present"

# ========== L1-C route / rules ==========
route=$(curl -fsS --max-time 30 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"task":"refactor auth concurrency","files":["a.cpp"],"hints":["quality"]}' \
  "$SMOKE_BASE/v1/route")
echo "$route" | python3 -c "
import sys,json
d=json.load(sys.stdin)
assert 'decision' in d and 'rules' in d
dec=d['decision']
for k in ('compression','retrieval','temperature','model','depth'):
  assert k in dec, k
" && smoke_pass "V5 v1/route" || smoke_bad "V5 route" "$(echo "$route"|head -c200)"

grules=$(curl -fsS --max-time 15 "${AUTH[@]}" "$SMOKE_BASE/v1/rules")
echo "$grules" | python3 -c "
import sys,json
arr=json.load(sys.stdin).get('rules') or []
assert arr and 'name' in arr[0]
" && smoke_pass "V6 GET v1/rules" || smoke_bad "V6 GET rules" "empty"

prules=$(curl -fsS --max-time 60 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"task":"deploy smoke","files":["tools/apex/mcp.cpp"],"manual":[]}' \
  "$SMOKE_BASE/v1/rules")
echo "$prules" | python3 -c "
import sys,json
d=json.load(sys.stdin)
assert 'rules' in d and isinstance(d['rules'], list)
" && smoke_pass "V7 POST v1/rules" || smoke_bad "V7 POST rules" "$(echo "$prules"|head -c200)"

smoke_skip "V8 v1/gate" "gate 已移除，改本地 agent"

# ========== L1-E/G 短路径（无 GGUF 也可）==========
code=$(curl -sS -o "$TMP/noin.txt" -w '%{http_code}' --max-time 15 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"model":"deepseek-flash","max_output_tokens":8}' "$SMOKE_BASE/v1/responses" || true)
echo "$code" | grep -q . 
body=$(cat "$TMP/noin.txt")
echo "$body" | python3 -c "
import sys,json
d=json.load(sys.stdin)
assert 'input' in str(d).lower() or d.get('error',{}).get('message')=='input required'
" && [[ "$code" = "400" ]] && smoke_pass "V16 responses missing input" || smoke_bad "V16" "code=$code body=$body"

code=$(curl -sS -o "$TMP/badj.txt" -w '%{http_code}' --max-time 15 "${AUTH[@]}" "${JSON[@]}" \
  -d 'not-json' "$SMOKE_BASE/v1/responses" || true)
[[ "$code" = "400" ]] && smoke_pass "V18 responses bad json" || smoke_bad "V18" "code=$code"

code=$(curl -sS -o "$TMP/nomsg.txt" -w '%{http_code}' --max-time 15 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"model":"deepseek-flash","max_tokens":8}' "$SMOKE_BASE/v1/chat/completions" || true)
body=$(cat "$TMP/nomsg.txt")
echo "$body" | python3 -c "
import sys,json
d=json.load(sys.stdin)
assert d.get('error',{}).get('message')=='messages required'
" && [[ "$code" = "400" ]] && smoke_pass "V21 chat missing messages" || smoke_bad "V21" "code=$code"

# V22 rate：默认 rate=0 → skip
smoke_skip "V22 rate 429" "rate=0 in example config"

# responses 非流式短路径：dryrun 也有 content（CI 始终跑）
resp=$(curl -fsS --max-time 90 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"model":"deepseek-v4-pro","input":"ping","max_output_tokens":16}' \
  "$SMOKE_BASE/v1/responses")
echo "$resp" | python3 -c "
import sys,json
d=json.load(sys.stdin)
assert d.get('object')=='response' and d.get('status')=='completed'
assert d['output'][0]['content'][0]['text']
" && smoke_pass "V15 v1/responses" || smoke_bad "V15 responses" "$(echo "$resp"|head -c200)"

# ========== 需 GGUF 的生成矩阵（无模型则 SKIP_NO_GGUF）==========
if [[ "$SMOKE_HAS_GGUF" -eq 1 ]]; then
  chat=$(curl -fsS --max-time 90 "${AUTH[@]}" "${JSON[@]}" \
    -d '{"model":"deepseek-flash","messages":[{"role":"user","content":"hi"}],"max_tokens":16}' \
    "$SMOKE_BASE/v1/chat/completions")
  echo "$chat" | python3 -c "
import sys,json
d=json.load(sys.stdin)
assert d['choices'][0]['message']['content']
assert d.get('model')
" && smoke_pass "V9 chat/completions" || smoke_bad "V9 chat" "$(echo "$chat"|head -c200)"

  chat2=$(curl -fsS --max-time 90 "${AUTH[@]}" "${JSON[@]}" \
    -d '{"model":"deepseek-flash","messages":[{"role":"user","content":"hi"}],"max_tokens":16}' \
    "$SMOKE_BASE/v1/chat")
  echo "$chat2" | python3 -c "import sys,json;assert json.load(sys.stdin)['choices'][0]['message']['content']" \
    && smoke_pass "V10 v1/chat" || smoke_bad "V10 chat" "fail"

  smoke_skip "V11 gate:false" "gate 已移除"
  smoke_skip "V12 chat gate short-circuit" "gate 已移除"
  smoke_skip "V17 responses gate short-circuit" "gate 已移除"
  smoke_skip "V13 gate conflict" "gate 已移除"
  smoke_skip "V13 chat after pack/refuse" "gate 已移除"

  chat4=$(curl -fsS --max-time 90 "${AUTH[@]}" "${JSON[@]}" \
    -d '{"model":"deepseek-flash","temperature":0.1,"messages":[{"role":"user","content":"hi"}],"max_tokens":8}' \
    "$SMOKE_BASE/v1/chat/completions")
  echo "$chat4" | python3 -c "
import sys,json
d=json.load(sys.stdin)
assert d.get('model')=='deepseek-flash'
assert d['choices'][0]['message']['content']
" && smoke_pass "V14 model/max_tokens echo" || smoke_bad "V14 model" "fail"

  # V19 chat stream
  curl -fsS --max-time 90 "${AUTH[@]}" \
    -H "Content-Type: application/json" -H "Accept: text/event-stream" \
    -d '{"model":"deepseek-flash","stream":true,"messages":[{"role":"user","content":"hi"}],"max_tokens":16}' \
    "$SMOKE_BASE/v1/chat/completions" >"$TMP/chat_sse.txt"
  python3 -c "
t=open('$TMP/chat_sse.txt',encoding='utf-8',errors='replace').read()
assert 'chat.completion.chunk' in t, t[:300]
assert 'data: [DONE]' in t, t[-200:]
" && smoke_pass "V19 chat stream" || smoke_bad "V19 stream" "fail"

  # V20 responses stream：必须 response.* 事件名
  curl -fsS --max-time 90 "${AUTH[@]}" \
    -H "Content-Type: application/json" -H "Accept: text/event-stream" \
    -d '{"model":"deepseek-v4-pro","stream":true,"input":"hi","max_output_tokens":16}' \
    "$SMOKE_BASE/v1/responses" >"$TMP/resp_sse.txt"
  python3 -c "
t=open('$TMP/resp_sse.txt',encoding='utf-8',errors='replace').read()
assert 'event: response.created' in t, t[:400]
assert 'event: response.completed' in t, t[-400:]
assert 'chat.completion' not in t or 'response.' in t
# 禁止整段冒充 chat.completion 事件流
assert 'event: chat.completion' not in t
" && smoke_pass "V20 responses stream" || smoke_bad "V20 stream" "fail"
else
  smoke_skip "V9 chat/completions" "SKIP_NO_GGUF"
  smoke_skip "V10 v1/chat" "SKIP_NO_GGUF"
  smoke_skip "V11 gate:false" "SKIP_NO_GGUF"
  smoke_skip "V12 chat gate short-circuit" "SKIP_NO_GGUF"
  smoke_skip "V13 pack/refuse chat" "SKIP_NO_GGUF"
  smoke_skip "V14 model/max_tokens" "SKIP_NO_GGUF"
  smoke_skip "V17 responses gate short-circuit" "SKIP_NO_GGUF"
  smoke_skip "V19 chat stream" "SKIP_NO_GGUF"
  smoke_skip "V20 responses stream" "SKIP_NO_GGUF"
fi

smoke_summary
echo OK
