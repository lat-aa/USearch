#!/usr/bin/env bash
# L1-agent：本地 agent 三路径（ok/delegate/truncated）+ 有界工具循环 + L1 缓存的确定性冒烟。
# 无需 GGUF / 无需上游 key：依托 SMOKE_AGENT_FIXTURE / SMOKE_UPSTREAM_FIXTURE seam。
# Usage: API_BIN=/root/usearch-build/api TOKEN=sk-default bash scripts/smoke_agent.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=scripts/smoke_common.sh
source "$ROOT/scripts/smoke_common.sh"

export SMOKE_HASH=1                       # 强制 hash 嵌入，免 2GB 权重加载
export SMOKE_BOOT_SECS="${SMOKE_BOOT_SECS:-120}"

TMP="${TMPDIR:-/tmp}/smoke_agent_$$"
mkdir -p "$TMP"

trap 'smoke_dump_log; smoke_teardown' ERR

post_chat() {
  curl -fsS --max-time 90 "${AUTH[@]}" "${JSON[@]}" \
    -d '{"model":"deepseek-flash","messages":[{"role":"user","content":"fixture probe"}]}' \
    "$SMOKE_BASE/v1/chat/completions"
}

agent_field() { # $1 = v1.agent.<field>
  curl -fsS --max-time 10 "${AUTH[@]}" "$SMOKE_BASE/ready" \
    | python3 -c "import sys,json;print(json.load(sys.stdin)['v1']['agent']['$1'])"
}

boot_mode() { # $1 = agent fixture ; $2 = upstream fixture
  smoke_teardown
  export SMOKE_AGENT_FIXTURE="$1"
  export SMOKE_UPSTREAM_FIXTURE="$2"
  smoke_boot
  smoke_auth_headers
}

# ---------- A1/A2/A3：ok 本地直答 + L1 缓存二次命中 ----------
boot_mode ok ""
out=$(post_chat)
echo "$out" | python3 -c "
import sys,json
assert json.load(sys.stdin)['choices'][0]['message']['content']=='fixture-ok', 'payload mismatch'
" && smoke_pass "A1 ok 本地直答 fixture-ok" || smoke_bad "A1 ok" "$(echo "$out"|head -c200)"

out2=$(post_chat) # 完全相同任务 → L1 精确命中
echo "$out2" | python3 -c "
import sys,json
assert json.load(sys.stdin)['choices'][0]['message']['content']=='fixture-ok'
" && smoke_pass "A2 L1 缓存二次命中" || smoke_bad "A2 cache" "$(echo "$out2"|head -c200)"

n=$(agent_field cache_l1)
[[ "$n" -ge 1 ]] && smoke_pass "A3 /ready cache_l1=$n" || smoke_bad "A3 cache_l1" "$n"

# ---------- A4/A5：有界工具循环 ----------
boot_mode tools ""
out3=$(post_chat)
echo "$out3" | python3 -c "
import sys,json
assert json.load(sys.stdin)['choices'][0]['message']['content']=='fixture-tools-ok'
" && smoke_pass "A4 有界工具循环收敛" || smoke_bad "A4 tools" "$(echo "$out3"|head -c200)"
r=$(agent_field rounds)
[[ "$r" -ge 1 ]] && smoke_pass "A5 /ready agent.rounds=$r" || smoke_bad "A5 rounds" "$r"

# ---------- A6：delegate → 上游兜底 ----------
boot_mode delegate "upstream-ok"
out4=$(post_chat)
echo "$out4" | python3 -c "
import sys,json
assert json.load(sys.stdin)['choices'][0]['message']['content']=='upstream-ok'
" && smoke_pass "A6 delegate→上游兜底" || smoke_bad "A6 delegate" "$(echo "$out4"|head -c200)"

# ---------- A7/A8：截断 → 上游兜底 ----------
boot_mode truncated "upstream-ok"
out5=$(post_chat)
echo "$out5" | python3 -c "
import sys,json
assert json.load(sys.stdin)['choices'][0]['message']['content']=='upstream-ok'
" && smoke_pass "A7 截断→上游兜底" || smoke_bad "A7 truncated" "$(echo "$out5"|head -c200)"
p=$(agent_field parsefail)
[[ "$p" -ge 1 ]] && smoke_pass "A8 /ready parsefail=$p" || smoke_bad "A8 parsefail" "$p"

# ---------- A9/A10：observe → 队列 → Worker → memory 双写 ----------
# 独立 sqlite：docs 从 0 起，Worker 写入可确定观测（不受历史库影响）。
export SMOKE_BASE_DB="${TMPDIR:-/tmp}/smoke_agent_$.sqlite"
rm -f "$SMOKE_BASE_DB"
boot_mode "" ""
unset SMOKE_BASE_DB
UNIQ="smoke-agent-obs-$(date +%s%N)"   # 内容派生的 obs id：每次唯一，保证产生新 memory
before=$(curl -fsS "${AUTH[@]}" "$SMOKE_BASE/ready" | python3 -c "import sys,json;print(json.load(sys.stdin)['docs'])")
obs=$(curl -fsS --max-time 15 "${MCP[@]}" \
  -d "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"observe\",\"arguments\":{\"payload\":{\"title\":\"$UNIQ\",\"summary\":\"fixture observation $UNIQ\",\"outcome\":\"ok\"}}}}" \
  "$SMOKE_BASE/mcp")
echo "$obs" | python3 -c "
import sys,json
txt=json.load(sys.stdin)['result']['content'][0]['text']
assert 'pending' in txt, txt
" && smoke_pass "A9 observe 仅入队（pending）" || smoke_bad "A9 observe" "$(echo "$obs"|head -c200)"

after="$before"
for _ in $(seq 1 30); do
  after=$(curl -fsS "${AUTH[@]}" "$SMOKE_BASE/ready" | python3 -c "import sys,json;print(json.load(sys.stdin)['docs'])")
  [[ "$after" -gt "$before" ]] && break
  sleep 1
done
[[ "$after" -gt "$before" ]] && smoke_pass "A10 Worker 落 memory（docs $before→$after）" \
  || smoke_bad "A10 worker" "docs $before→$after"

smoke_teardown
smoke_summary
echo OK
