#!/usr/bin/env bash
# L2：Apex MCP 回合矩阵（rules/decide/cost/gate/observe/aliases/stats）。
# 从 deploy/k3s/depth.sh 抽出可本地化断言；部署探测仍由 depth.sh 负责。
# Usage: API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_apex.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=scripts/smoke_common.sh
source "$ROOT/scripts/smoke_common.sh"

trap 'smoke_dump_log; smoke_teardown' ERR
smoke_boot
smoke_auth_headers
trap 'smoke_teardown' EXIT

# initialize（后续 tools/call 依赖会话习惯；本实现无状态，仍走一遍）
curl -fsS --max-time 15 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"smoke-apex","version":"0"}}}' \
  "$SMOKE_BASE/mcp" >/dev/null
curl -sS --max-time 10 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}' "$SMOKE_BASE/mcp" >/dev/null || true

# A1 rules → decide → cost
rules=$(curl -fsS --max-time 60 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"rules","arguments":{"task":"deploy apex smoke","files":["tools/apex/mcp.cpp"],"manual":[]}}}' \
  "$SMOKE_BASE/mcp")
echo "$rules" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
for k in ('totalRules','matched','naiveTokens','selectedTokens','optimizedTokens','entries'):
  assert k in t, k
assert t['entries'] and 'id' in t['entries'][0]
" && smoke_pass "A1 mcp rules" || smoke_bad "A1 rules" "fail"

decide=$(curl -fsS --max-time 30 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"decide","arguments":{"task":"refactor auth for concurrency and security","files":["a.cpp","b.cpp"],"hints":["quality"]}}}' \
  "$SMOKE_BASE/mcp")
echo "$decide" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
for k in ('model','depth','retrieval','compression','confidence','reasons'):
  assert k in t, k
" && smoke_pass "A1 mcp decide" || smoke_bad "A1 decide" "fail"

cost=$(curl -fsS --max-time 60 "${MCP[@]}" \
  -H "X-Apex-Actual-Model: Grok 4.6" -H "X-Apex-Actual-Model-Source: reported" \
  -d '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"cost","arguments":{"task":"say hi","files":[],"manual":[]}}}' \
  "$SMOKE_BASE/mcp")
echo "$cost" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert t['actual_model']=='Grok 4.6' and 'stack' in t
assert 'turn' in t and 'gate' in t['turn']
tr=t['turn']
c=tr.get('corpus') or ''
msgs=tr.get('prompt') or []
assert isinstance(msgs,list) and msgs and msgs[0].get('role')=='system'
assert msgs[0]['content'][0]['type']=='text' and msgs[0]['content'][0].get('text')
p=''.join(part.get('text','') for m in msgs for part in (m.get('content') or []) if isinstance(part,dict))
assert c and c.startswith('## prompt\n')
assert '## rules' not in c and '## kept' not in c and '## pack' not in c
assert 'Local knowledge JSON follows.' in p
assert 'Local knowledge JSON follows.' not in c
assert '路由' in c
assert '\"body\": \"...\"' not in p and '\"body\":\"...\"' not in p
assert tr.get('source') == 'rebuild'
assert '(none)' not in c
assert tr.get('rules') and tr['rules'][0].get('body')
assert tr.get('clip') and tr['clip'][0].get('body')
assert '\"pack\"' not in p or tr.get('packn', 0) > 0
assert tr.get('kept') == t.get('optimized_tokens')
" && smoke_pass "A1 mcp cost stack+turn.corpus" || smoke_bad "A1 cost" "fail"

nocost=$(curl -fsS --max-time 20 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"cost","arguments":{"task":"x"}}}' \
  "$SMOKE_BASE/mcp")
echo "$nocost" | python3 -c "
import sys,json
r=json.load(sys.stdin)['result']
assert r.get('isError') is True and 'actual_model' in r['content'][0]['text']
" && smoke_pass "A1 cost requires actual_model" || smoke_bad "A1 cost require" "fail"

# A2 cursor-state 优先于头
prio=$(curl -fsS --max-time 60 "${MCP[@]}" \
  -H "X-Apex-Actual-Model: from-header" \
  -d '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"cost","arguments":{"task":"x","actual_model":"from-hook","actual_model_source":"cursor-state"}}}' \
  "$SMOKE_BASE/mcp")
echo "$prio" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert t['actual_model']=='from-hook'
" && smoke_pass "A2 actual_model cursor-state > header" || smoke_bad "A2 priority" "fail"

# A3 Codex 别名
alias=$(curl -fsS --max-time 60 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"resolve-rules","arguments":{"task":"x","files":[],"manual":[]}}}' \
  "$SMOKE_BASE/mcp")
echo "$alias" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert 'totalRules' in t
" && smoke_pass "A3 alias resolve-rules" || smoke_bad "A3 resolve-rules" "fail"

list=$(curl -fsS --max-time 30 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"list-rules","arguments":{}}}' \
  "$SMOKE_BASE/mcp")
echo "$list" | python3 -c "
import sys,json
arr=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert isinstance(arr,list) and arr
" && smoke_pass "A3 alias list-rules" || smoke_bad "A3 list-rules" "fail"

rname=$(echo "$list" | python3 -c "import sys,json;arr=json.loads(json.load(sys.stdin)['result']['content'][0]['text']);print(arr[0]['name'])")
getr=$(curl -fsS --max-time 30 "${MCP[@]}" \
  -d "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\",\"params\":{\"name\":\"get-rule\",\"arguments\":{\"name\":\"$rname\"}}}" \
  "$SMOKE_BASE/mcp")
echo "$getr" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert t.get('name') and 'body' in t
" && smoke_pass "A3 alias get-rule" || smoke_bad "A3 get-rule" "fail"

# A4 gate → cost
gturn=$(curl -fsS --max-time 120 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":30,"method":"tools/call","params":{"name":"gate","arguments":{"task":"how to name a C++ flag without underscore","files":[]}}}' \
  "$SMOKE_BASE/mcp")
gstatus=$(echo "$gturn" | python3 -c "import sys,json;t=json.loads(json.load(sys.stdin)['result']['content'][0]['text']);print(t.get('status',''))")
costg=$(curl -fsS --max-time 60 "${MCP[@]}" \
  -H "X-Apex-Actual-Model: apex-gate" -H "X-Apex-Actual-Model-Source: reported" \
  -d '{"jsonrpc":"2.0","id":31,"method":"tools/call","params":{"name":"cost","arguments":{"task":"how to name a C++ flag without underscore","files":[],"manual":[]}}}' \
  "$SMOKE_BASE/mcp")
echo "$costg" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
tr=t.get('turn') or {}
assert tr.get('gate') in ('answered','pack','refuse','none'), tr
if tr.get('gate')=='answered':
  assert tr.get('saved')==1, tr
c=tr.get('corpus') or ''
msgs=tr.get('prompt') or []
assert isinstance(msgs,list) and msgs
p=''.join(part.get('text','') for m in msgs for part in (m.get('content') or []) if isinstance(part,dict))
assert c and c.startswith('## prompt\n')
assert '## rules' not in c and '## kept' not in c
assert 'Local knowledge JSON follows.' in p
assert '\"body\": \"...\"' not in p and '\"body\":\"...\"' not in p
assert tr.get('source') in ('injected', 'rebuild')
assert '(none)' not in c
" && smoke_pass "A4 cost.turn after gate (status=$gstatus)" || smoke_bad "A4 cost.turn gate" "status=$gstatus"

# A5 observe/save
obs=$(curl -fsS --max-time 30 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":22,"method":"tools/call","params":{"name":"observe","arguments":{"title":"apex-smoke","summary":"queue worker smoke","outcome":"ok"}}}' \
  "$SMOKE_BASE/mcp")
oid=$(echo "$obs" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert t.get('ok') is True and t.get('status')=='pending' and t.get('id')
print(t['id'])
") && smoke_pass "A5 observe enqueue" || smoke_bad "A5 observe" "$(echo "$obs"|head -c200)"

# observe 后 worker 后台蒸馏 chat 持 Encoder 锁（embed/chat 共用 mutex），cost 可能等待 ~75s
costq=$(curl -fsS --max-time 180 "${MCP[@]}" \
  -H "X-Apex-Actual-Model: apex-q" -H "X-Apex-Actual-Model-Source: reported" \
  -d '{"jsonrpc":"2.0","id":23,"method":"tools/call","params":{"name":"cost","arguments":{"task":"after observe","files":[],"manual":[]}}}' \
  "$SMOKE_BASE/mcp")
echo "$costq" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
q=t.get('turn',{}).get('queued') or []
# queued 可能是 list 或已消费；至少 turn 存在
assert 'turn' in t
" && smoke_pass "A5 cost after observe" || smoke_bad "A5 cost queued" "fail"

# A6 stats.sh 七块
# observe 后 worker 后台蒸馏 chat 持 Encoder 锁（embed/chat 共用 mutex），cost 可能等待 ~75s
hdr=$(curl -fsS --max-time 180 "${MCP[@]}" \
  -H "X-Apex-Actual-Model: deepseek-v4-pro" -H "X-Apex-Actual-Model-Source: codex-config" \
  -d '{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"cost","arguments":{"task":"refactor large system design architecture concurrency","files":["a.cpp","b.cpp","c.cpp"],"hints":["quality"]}}}' \
  "$SMOKE_BASE/mcp")
STACK_JSON=$(echo "$hdr" | python3 -c "import sys,json;t=json.loads(json.load(sys.stdin)['result']['content'][0]['text']);print(json.dumps(t))")
python3 - <<PY "$STACK_JSON" "$ROOT"
import json,sys,subprocess,tempfile,os
c=json.loads(sys.argv[1]); root=sys.argv[2]
st=c["stack"]; d=c["decision"]; tr=c.get("turn") or {}
corpus=tr.get("corpus") or ""
fd, path = tempfile.mkstemp(prefix="apex-corpus-", suffix=".txt")
os.write(fd, corpus.encode("utf-8")); os.close(fd)
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
"-Nanbeige",st["nanbeige"],"-USearch",st["usearch"],"-Sqlite",st["sqlite"],
"-Gate",str(tr.get("gate") or ""),"-Cache",str(tr.get("cache") or ""),
"-Retain",str(tr.get("retain") if tr.get("retain") is not None else d["compression"]),
"-CtxNaive",str(tr.get("naive") if tr.get("naive") is not None else c["naive_tokens"]),
"-CtxPicked",str(tr.get("picked") if tr.get("picked") is not None else c["selected_tokens"]),
"-CtxKept",str(tr.get("kept") if tr.get("kept") is not None else c["optimized_tokens"]),
"-PackTok",str(tr.get("packtok") if tr.get("packtok") is not None else ""),
"-PackN",str(tr.get("packn") if tr.get("packn") is not None else ""),
"-PromptSource",str(tr.get("source") or ""),
"-CorpusFile",path]
if "saved" in tr:
  cmd += ["-Saved",str(tr["saved"])]
if "local" in tr:
  cmd += ["-Local",str(tr["local"])]
out=subprocess.check_output(cmd,text=True)
os.unlink(path)
assert "🔖 决策 Nanbeige4.1" in out
assert "📦 上下文" in out
assert "naive" in out and "kept" in out and "gatePack" in out
assert "注入" in out
assert "## prompt" in out
assert "## rules" not in out and "## kept" not in out
# 统计块贴人读 corpus，禁止再刷 knowledge JSON
assert "Local knowledge JSON follows." not in out
assert "路由" in out
assert '"body": "..."' not in out and '"body":"..."' not in out
print(out)
PY
smoke_pass "A6 stats.sh seven-line+prompt-only"

smoke_summary
echo OK
