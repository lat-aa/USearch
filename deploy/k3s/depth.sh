#!/bin/bash
# 部署后深度场景覆盖：鉴权 / MCP /v1 / stack / 别名 / SSE / 向量 CRUD
# Usage: BASE=http://api.ya.com bash deploy/k3s/depth.sh
set -euo pipefail
TOKEN="${TOKEN:-sk-default}"
BASE="${BASE:-http://api.ya.com}"
NP="${NP:-http://127.0.0.1:30088}"
ok=0; fail=0
pass() { echo "PASS  $1"; ok=$((ok+1)); }
bad()  { echo "FAIL  $1 — $2"; fail=$((fail+1)); }

wait_up() {
  local url=$1
  for i in $(seq 1 60); do
    if curl -fsS --max-time 3 "$url/alive" >/dev/null 2>&1; then return 0; fi
    sleep 2
  done
  return 1
}

echo "== rollout =="
k3s kubectl -n usearch rollout status deploy/api --timeout=180s || true
k3s kubectl -n usearch get pods,svc,ingress -o wide

if wait_up "$BASE"; then
  USE=$BASE
  echo "using Ingress $USE"
elif wait_up "$NP"; then
  USE=$NP
  echo "using NodePort $USE (Ingress down)"
else
  echo "FATAL neither Ingress nor NodePort alive"
  exit 1
fi

AUTH=(-H "Authorization: Bearer $TOKEN")
JSON=(-H "Content-Type: application/json" -H "Accept: application/json")
MCP=(-H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream")

if curl -fsS --max-time 8 "$USE/alive" | grep -q '"ok":true'; then pass "alive"; else bad "alive" "not ok"; fi
ready=$(curl -fsS --max-time 15 "${AUTH[@]}" "$USE/ready" || true)
echo "$ready" | python3 -c "import sys,json;d=json.load(sys.stdin);assert d.get('ok') and d.get('model') is True and d.get('dim',0)>0" \
  && pass "ready model+dim" || bad "ready" "$ready"

code=$(curl -sS -o /tmp/uauth.txt -w '%{http_code}' --max-time 8 "$USE/v1/models" || true)
[ "$code" = "401" ] && pass "auth deny no bearer" || bad "auth deny" "code=$code"
code=$(curl -sS -o /tmp/ubad.txt -w '%{http_code}' --max-time 8 -H "Authorization: Bearer wrong" "$USE/v1/models" || true)
[ "$code" = "401" ] && pass "auth deny bad token" || bad "auth deny bad" "code=$code"

models=$(curl -fsS --max-time 10 "${AUTH[@]}" "$USE/v1/models")
echo "$models" | python3 -c "import sys,json;ids={x['id'] for x in json.load(sys.stdin)['data']};assert 'deepseek-flash' in ids and 'deepseek-v4-pro' in ids;assert any('Nanbeige' in i for i in ids)" \
  && pass "v1/models catalog" || bad "v1/models" "$models"

emb=$(curl -fsS --max-time 60 "${AUTH[@]}" "${JSON[@]}" -d '{"input":"depth-test embed"}' "$USE/v1/embeddings")
echo "$emb" | python3 -c "import sys,json;e=json.load(sys.stdin)['data'][0]['embedding'];assert len(e)>=256" \
  && pass "v1/embeddings" || bad "v1/embeddings" "short"

resp=$(curl -fsS --max-time 90 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"model":"deepseek-v4-pro","input":"ping","max_output_tokens":16}' "$USE/v1/responses")
echo "$resp" | python3 -c "import sys,json;d=json.load(sys.stdin);assert d.get('object')=='response' and d.get('status')=='completed';assert d['output'][0]['content'][0]['text']" \
  && pass "v1/responses" || bad "v1/responses" "$(echo "$resp"|head -c200)"

chat=$(curl -fsS --max-time 90 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"model":"deepseek-flash","messages":[{"role":"user","content":"hi"}],"max_tokens":16}' "$USE/v1/chat/completions")
echo "$chat" | python3 -c "import sys,json;d=json.load(sys.stdin);assert d['choices'][0]['message']['content']" \
  && pass "v1/chat/completions" || bad "v1/chat" "$(echo "$chat"|head -c200)"

route=$(curl -fsS --max-time 30 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"task":"refactor auth concurrency","files":["a.cpp"],"hints":["quality"]}' "$USE/v1/route")
echo "$route" | python3 -c "import sys,json;d=json.load(sys.stdin);assert 'decision' in d and 'rules' in d" \
  && pass "v1/route" || bad "v1/route" "$(echo "$route"|head -c200)"

init=$(curl -fsS --max-time 15 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"depth","version":"0"}}}' \
  "$USE/mcp")
echo "$init" | python3 -c "import sys,json;assert json.load(sys.stdin)['result']['protocolVersion']" \
  && pass "mcp initialize" || bad "mcp initialize" "$init"

code=$(curl -sS -o /tmp/mcp_initd.txt -w '%{http_code}' --max-time 10 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}' "$USE/mcp" || true)
[ "$code" = "202" ] && pass "mcp initialized 202" || bad "mcp initialized" "code=$code"

curl -fsS --max-time 5 -H "Accept: text/event-stream" "${AUTH[@]}" "$USE/mcp" >/tmp/mcp_sse.txt || true
if grep -q '^data: {' /tmp/mcp_sse.txt 2>/dev/null; then
  bad "mcp GET SSE" "leaked data: {"
else
  pass "mcp GET SSE comment-only"
fi

tlist=$(curl -fsS --max-time 15 "${MCP[@]}" -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' "$USE/mcp")
echo "$tlist" | python3 -c "
import sys,json
names={t['name'] for t in json.load(sys.stdin)['result']['tools']}
need={'rules','decide','cost','gate','observe','resolve-rules','list-rules','get-rule','search','upsert','delete','recall'}
miss=need-names
assert not miss, miss
" && pass "mcp tools/list apex+aliases" || bad "mcp tools/list" "missing"

rules=$(curl -fsS --max-time 60 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"rules","arguments":{"task":"deploy depth test","files":["cpp/mcp.cpp"],"manual":[]}}}' \
  "$USE/mcp")
echo "$rules" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
for k in ('totalRules','matched','naiveTokens','selectedTokens','optimizedTokens','entries'):
  assert k in t, k
assert t['entries'] and 'id' in t['entries'][0]
" && pass "mcp rules envelope" || bad "mcp rules" "fail"

decide=$(curl -fsS --max-time 30 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"decide","arguments":{"task":"refactor auth for concurrency and security","files":["a.cpp","b.cpp"],"hints":["quality"]}}}' \
  "$USE/mcp")
echo "$decide" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
for k in ('model','depth','retrieval','compression','confidence','reasons'):
  assert k in t, k
" && pass "mcp decide" || bad "mcp decide" "fail"

cost=$(curl -fsS --max-time 60 "${MCP[@]}" \
  -H "X-Apex-Actual-Model: Grok 4.6" -H "X-Apex-Actual-Model-Source: reported" \
  -d '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"cost","arguments":{"task":"say hi","files":[],"manual":[]}}}' \
  "$USE/mcp")
echo "$cost" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert t['actual_model']=='Grok 4.6' and 'stack' in t
" && pass "mcp cost stack" || bad "mcp cost stack" "fail"

nocost=$(curl -fsS --max-time 20 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"cost","arguments":{"task":"x"}}}' \
  "$USE/mcp")
echo "$nocost" | python3 -c "
import sys,json
r=json.load(sys.stdin)['result']
assert r.get('isError') is True and 'actual_model' in r['content'][0]['text']
" && pass "mcp cost requires actual_model" || bad "mcp cost require" "fail"

prio=$(curl -fsS --max-time 60 "${MCP[@]}" \
  -H "X-Apex-Actual-Model: from-header" \
  -d '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"cost","arguments":{"task":"x","actual_model":"from-hook","actual_model_source":"cursor-state"}}}' \
  "$USE/mcp")
echo "$prio" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert t['actual_model']=='from-hook'
" && pass "actual_model cursor-state > header" || bad "actual_model priority" "fail"

hdr=$(curl -fsS --max-time 60 "${MCP[@]}" \
  -H "X-Apex-Actual-Model: deepseek-v4-pro" -H "X-Apex-Actual-Model-Source: codex-config" \
  -d '{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"cost","arguments":{"task":"refactor large system design architecture concurrency","files":["a.cpp","b.cpp","c.cpp"],"hints":["quality"]}}}' \
  "$USE/mcp")
echo "$hdr" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert t['actual_model']=='deepseek-v4-pro'
assert t['stack']['nanbeige'] and t['stack']['usearch'] and t['stack']['sqlite']
" && pass "header actual_model + rich task stack" || bad "header cost" "fail"

alias=$(curl -fsS --max-time 60 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"resolve-rules","arguments":{"task":"x","files":[],"manual":[]}}}' \
  "$USE/mcp")
echo "$alias" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert 'totalRules' in t
" && pass "alias resolve-rules" || bad "alias resolve-rules" "fail"

uid="depth-$(date +%s)"
up=$(curl -fsS --max-time 60 "${MCP[@]}" \
  -d "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\",\"params\":{\"name\":\"upsert\",\"arguments\":{\"id\":\"$uid\",\"text\":\"depth test vector doc about authentication\"}}}" \
  "$USE/mcp")
echo "$up" | python3 -c "import sys,json;assert json.load(sys.stdin)['result'].get('isError') is False" \
  && pass "mcp upsert" || bad "mcp upsert" "fail"

sr=$(curl -fsS --max-time 60 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"search","arguments":{"text":"authentication","k":3}}}' \
  "$USE/mcp")
echo "$sr" | python3 -c "
import sys,json
arr=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert isinstance(arr,list) and len(arr)>=1
" && pass "mcp search hits" || bad "mcp search" "fail"

dl=$(curl -fsS --max-time 30 "${MCP[@]}" \
  -d "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\",\"params\":{\"name\":\"delete\",\"arguments\":{\"id\":\"$uid\"}}}" \
  "$USE/mcp")
echo "$dl" | python3 -c "import sys,json;assert json.load(sys.stdin)['result'].get('isError') is False" \
  && pass "mcp delete" || bad "mcp delete" "fail"

ghttp=$(curl -fsS --max-time 120 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"task":"how to name a C++ flag without underscore"}' "$USE/v1/gate")
echo "$ghttp" | python3 -c "
import sys,json
d=json.load(sys.stdin)
assert d.get('status') in ('answered','pack','refuse'), d
assert 'policyFingerprint' in d and 'answerConfidence' in d
" && pass "v1/gate status" || bad "v1/gate" "$(echo "$ghttp"|head -c200)"

# 冲突记忆：含禁止命名 token，应 refuse/pack 且不得 answered
curl -fsS --max-time 60 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":20,"method":"tools/call","params":{"name":"upsert","arguments":{"id":"conflict-depth","text":"use model_ready please"}}}' \
  "$USE/mcp" >/dev/null || true
gconf=$(curl -fsS --max-time 120 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"task":"add model_ready flag to encoder"}' "$USE/v1/gate")
echo "$gconf" | python3 -c "
import sys,json
d=json.load(sys.stdin)
assert d.get('status') in ('refuse','pack'), d
assert d.get('status') != 'answered'
" && pass "gate conflict fail-closed" || bad "gate conflict" "$(echo "$gconf"|head -c200)"
curl -fsS --max-time 30 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":21,"method":"tools/call","params":{"name":"delete","arguments":{"id":"conflict-depth"}}}' \
  "$USE/mcp" >/dev/null || true

obs=$(curl -fsS --max-time 30 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":22,"method":"tools/call","params":{"name":"observe","arguments":{"title":"depth","summary":"queue worker smoke","outcome":"ok"}}}' \
  "$USE/mcp")
echo "$obs" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert t.get('ok') is True and t.get('status')=='pending' and t.get('id')
" && pass "mcp observe enqueue" || bad "mcp observe" "$(echo "$obs"|head -c200)"
sleep 3
# Worker 异步；不强制 done，仅确认入队 API

ROOT="$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)"
STACK_JSON=$(echo "$hdr" | python3 -c "import sys,json;t=json.loads(json.load(sys.stdin)['result']['content'][0]['text']);print(json.dumps(t))" )
python3 - <<PY "$STACK_JSON" "$ROOT"
import json,sys,subprocess
c=json.loads(sys.argv[1]); root=sys.argv[2]
st=c["stack"]; d=c["decision"]
cmd=["bash",root+"/scripts/stats.sh",
"-Total",str(c["totalRules"]),"-Matched",str(c["matched"]),
"-Naive",str(c["naive_tokens"]),"-Optimized",str(c["optimized_tokens"]),
"-Selected",str(c["selected_tokens"]),"-Ids","default",
"-Model",str(d["model"]),"-Depth",str(d["depth"]),"-Retrieval",str(d["retrieval"]),
"-Confidence",str(d["confidence"]),"-Compression",str(d["compression"]),
"-Reason",(d.get("reasons") or [""])[0],
"-NaiveCost",str(c["naive_input_cost"]),"-OptimizedCost",str(c["optimized_input_cost"]),
"-SavedCost",str(c["saved_input_cost"]),"-OutputCost","0","-TotalCost",str(c["total_cost"]),
"-CacheHit","false","-Peak","false","-ViaV1","false",
"-CostModel",str(c["model"]),"-ActualModel",str(c["actual_model"]),
"-ActualModelSource",str(c["actual_model_source"]),
"-Nanbeige",st["nanbeige"],"-Usearch",st["usearch"],"-Sqlite",st["sqlite"]]
out=subprocess.check_output(cmd,text=True)
print(out)
assert "🔖 决策 Nanbeige4.1" in out
PY
pass "stats.sh six-line"

echo
echo "======== SUMMARY pass=$ok fail=$fail ========"
[ "$fail" -eq 0 ]
