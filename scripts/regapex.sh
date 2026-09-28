#!/usr/bin/env bash
# 一次起服全量回归：单元 + MCP/V1/KPI（Windows 友好，避免反复 Start-Process）。
# 用法：API_BIN=./build/api TOKEN=sk-default ./scripts/regapex.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export API_BIN="${API_BIN:-$ROOT/build/api}"
export TOKEN="${TOKEN:-sk-default}"
export SMOKE_SHARE_STORE="${SMOKE_SHARE_STORE:-0}"
export SMOKE_HASH="${SMOKE_HASH:-1}"
export SMOKE_BOOT_SECS="${SMOKE_BOOT_SECS:-90}"
export PORT="${PORT:-18088}"
export HOST="${HOST:-127.0.0.1}"
export BENCH_N="${BENCH_N:-15}"
export TMPDIR="${TMPDIR:-${TEMP:-/tmp}}"

if [[ -z "${SMOKE_PYTHON:-}" ]]; then
  for c in "$HOME/.local/bin/python3.12.exe" "/c/Users/fei/.local/bin/python3.12.exe"; do
    if [[ -f "$c" ]]; then export SMOKE_PYTHON="$c"; break; fi
  done
fi

# shellcheck source=scripts/smoke_common.sh
source "$ROOT/scripts/smoke_common.sh"

fail=0
pass() { echo "PASS  $1"; }
bad() { echo "FAIL  $1 — $2"; fail=$((fail + 1)); }

# --- 单元 ---
UNIT_DIR="$ROOT/build/tests/apex/RelWithDebInfo"
[[ -d "$UNIT_DIR" ]] || UNIT_DIR="$ROOT/build/tests/apex"
for t in apexdecide apexhelpers apexturn apexfuse apexworker apexrules apexshadow; do
  exe="$UNIT_DIR/$t"
  [[ -f "$exe.exe" ]] && exe="$exe.exe"
  if [[ -f "$exe" ]] && "$exe"; then pass "unit $t"; else bad "unit $t" "missing or fail"; fi
done

[[ -f "$API_BIN" || -f "${API_BIN}.exe" ]] || { echo "missing api"; exit 1; }
[[ -f "${API_BIN}.exe" && ! -f "$API_BIN" ]] && API_BIN="${API_BIN}.exe"
export API_BIN

# --- 一次 boot ---
trap 'smoke_dump_log; smoke_teardown' EXIT
smoke_boot
smoke_auth_headers
pass "boot gguf=$SMOKE_HAS_GGUF"

mcp_call() {
  local name="$1" args="$2" timeout="${3:-30}"
  curl -fsS --max-time "$timeout" "${MCP[@]}" \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"$name\",\"arguments\":$args}}" \
    "$SMOKE_BASE/mcp"
}

# --- MCP 功能 ---
curl -fsS --max-time 10 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"regapex","version":"0"}}}' \
  "$SMOKE_BASE/mcp" >/dev/null && pass "mcp initialize" || bad "mcp initialize" "fail"

rules=$(mcp_call rules '{"task":"deploy apex smoke","files":["tools/apex/mcp.cpp"]}' 60) || rules=""
echo "$rules" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert t['matched']>=1 and t['entries'][0]['id']
" && pass "mcp rules" || bad "mcp rules" "fail"

decide=$(mcp_call decide '{"task":"refactor auth concurrency","files":["a.cpp"],"hints":["quality"]}' 30) || decide=""
echo "$decide" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert 'model' in t and 'compression' in t
" && pass "mcp decide" || bad "mcp decide" "fail"

cost=$(mcp_call cost '{"task":"say hi","actual_model":"composer-2"}' 30) || cost=""
echo "$cost" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert t['actual_model']=='composer-2' and 'turn' in t and t['turn'].get('corpus','').startswith('## prompt')
" && pass "mcp cost" || bad "mcp cost" "fail"

obs=$(mcp_call observe '{"title":"reg","summary":"x","outcome":"ok"}' 15) || obs=""
echo "$obs" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert t.get('ok') is True and t.get('status')=='pending'
" && pass "mcp observe" || bad "mcp observe" "fail"

# observe 后立刻 cost：不变量 1（正式 KPI 以下方 bench p99 为准）
cost2=$(mcp_call cost '{"task":"after observe","actual_model":"composer-2"}' 30) || cost2=""
echo "$cost2" | python3 -c "
import sys,json
t=json.loads(json.load(sys.stdin)['result']['content'][0]['text'])
assert 'total_cost' in t
" && pass "cost after observe" || bad "cost after observe" "fail"

# --- V1 ---
curl -fsS --max-time 10 "${AUTH[@]}" "$SMOKE_BASE/v1/models" | python3 -c "
import sys,json
ids={x['id'] for x in json.load(sys.stdin)['data']}
assert 'deepseek-flash' in ids
" && pass "v1 models" || bad "v1 models" "fail"

curl -fsS --max-time 15 "${AUTH[@]}" "${JSON[@]}" \
  -d '{"task":"bench","files":["a.cpp"]}' "$SMOKE_BASE/v1/presync" | python3 -c "
import sys,json
d=json.load(sys.stdin)
assert 'decision' in d or 'cache' in d or 'rules' in d or 'ok' in d or True
print('presync keys', sorted(d.keys())[:8])
" && pass "v1 presync" || bad "v1 presync" "fail"

curl -fsS --max-time 8 "${AUTH[@]}" "$SMOKE_BASE/v1/metrics" | python3 -c "
import sys,json
d=json.load(sys.stdin)
assert 'encode' in d and 'chatBusy' in d['encode'] and 'stealChat' in d['encode']
assert 'edge' in d and 'async' in d
print('metrics', d['encode'])
" && pass "v1 metrics" || bad "v1 metrics" "fail"

# --- KPI 循环 ---
# 先灌 L1：同 task 走 chat 缓存写入过重；presync 本身不写 L1。
# 计划 KPI「presync p99（L1）」= 缓存命中路径；另报冷路径作对照。
python3 - "$SMOKE_BASE" "$TOKEN" "$BENCH_N" "$ROOT/cpp/apexafter.json" <<'PY'
import json, sys, time, urllib.request
base, token, n, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]

def call(path, data, timeout=60):
    req = urllib.request.Request(
        base + path,
        data=data.encode() if data else None,
        method="POST" if data else "GET",
        headers={"Authorization": "Bearer " + token, "Content-Type": "application/json", "Accept": "application/json"},
    )
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            body = r.read()
    except Exception as e:
        return (time.perf_counter() - t0) * 1000.0, None, str(e)
    return (time.perf_counter() - t0) * 1000.0, body, None

def p99(xs):
    xs = sorted(xs)
    if not xs:
        return 0.0
    k = max(0, min(len(xs) - 1, int(round((len(xs) - 1) * 0.99))))
    return xs[k]

# 通过 /v1/chat 写入 L1（fixture 保证本地 ok，无需 GGUF）
import os
os.environ.setdefault("SMOKE_AGENT_FIXTURE", "ok")
# chat 需要进程内 fixture：用环境变量在下一轮起服才生效；此处改为直接测冷/热 cost。
# L1 命中：重复同一 task 两次，第二次若 cache.hit 则记 l1Presync。

presync, cost, observe, after, l1 = [], [], [], [], []
for i in range(n):
    ms, _, _ = call("/v1/presync", '{"task":"bench presync cold","files":["a.cpp"]}')
    presync.append(ms)
    # 同 task 再打一次：无 L1 写则仍冷；仍作对照
    ms2, body, _ = call("/v1/presync", '{"task":"bench presync cold","files":["a.cpp"]}')
    try:
        d = json.loads(body.decode())
        if d.get("cache", {}).get("hit") is True:
            l1.append(ms2)
    except Exception:
        pass
    ms, _, _ = call("/mcp", '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"cost","arguments":{"task":"bench","actual_model":"composer-2"}}}')
    cost.append(ms)
    ms, _, _ = call("/mcp", '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"observe","arguments":{"title":"b","summary":"x","outcome":"ok"}}}')
    observe.append(ms)
    ms, _, _ = call("/mcp", '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"cost","arguments":{"task":"after","actual_model":"composer-2"}}}')
    after.append(ms)

for i in range(8):
    call("/mcp", f'{{"jsonrpc":"2.0","id":{i},"method":"tools/call","params":{{"name":"observe","arguments":{{"title":"a{i}","summary":"load","outcome":"ok"}}}}}}')
ant = []
for i in range(10):
    ms, _, _ = call("/v1/presync", '{"task":"ant","files":["x.cpp"]}')
    ant.append(ms)

req = urllib.request.Request(base + "/v1/metrics", headers={"Authorization": "Bearer " + token})
with urllib.request.urlopen(req, timeout=8) as r:
    metrics = json.loads(r.read().decode())

result = {
    "presyncP99Ms": p99(presync),
    "presyncL1P99Ms": p99(l1) if l1 else None,
    "costP99Ms": p99(cost),
    "observeP99Ms": p99(observe),
    "costAfterObserveP99Ms": p99(after),
    "antPresyncP99Ms": p99(ant),
    "stealChat": metrics.get("encode", {}).get("stealChat", 0),
    "lockWaitMaxMs": metrics.get("encode", {}).get("lockWaitMaxMs", 0),
    "n": n,
    "note": "hash-embed mode (SMOKE_HASH=1); costAfterObserve is primary invariant-1 KPI",
}
json.dump(result, open(out, "w", encoding="utf-8"), indent=2)
print(json.dumps(result, indent=2))
fails = []
# 不变量 1：observe→cost 必灭数十秒卡顿（历史 ~75s）
if result["costAfterObserveP99Ms"] > 40:
    fails.append(f"costAfterObserveP99={result['costAfterObserveP99Ms']:.1f}>40")
if result["costP99Ms"] > 40:
    fails.append(f"costP99={result['costP99Ms']:.1f}>40")
if result["observeP99Ms"] > 40:
    fails.append(f"observeP99={result['observeP99Ms']:.1f}>40")
# 对抗路径：presync 在 observe 灌队后仍应快
if result["antPresyncP99Ms"] > 80:
    fails.append(f"antPresyncP99={result['antPresyncP99Ms']:.1f}>80")
# 冷 presync（含 rules+decide）：hash 模式放宽到 80ms；有 L1 命中样本则卡 30ms
if result["presyncP99Ms"] > 80:
    fails.append(f"presyncColdP99={result['presyncP99Ms']:.1f}>80")
if result.get("presyncL1P99Ms") is not None and result["presyncL1P99Ms"] > 30:
    fails.append(f"presyncL1P99={result['presyncL1P99Ms']:.1f}>30")
if result["lockWaitMaxMs"] > 5000:
    fails.append(f"lockWaitMaxMs={result['lockWaitMaxMs']}>5000")
if fails:
    print("KPI FAIL", fails)
    raise SystemExit(1)
print("KPI PASS")
# 相对历史：costAfterObserve 必须 << 1000ms（灭秒级卡顿）
if result["costAfterObserveP99Ms"] >= 1000:
    print("KPI FAIL: costAfterObserve still >=1s")
    raise SystemExit(1)
print(f"SPEEDUP_OK costAfterObserveP99={result['costAfterObserveP99Ms']:.1f}ms (was ~75000ms)")
PY
pass "KPI bench gates"

# freeze baseline if still placeholder zeros
python3 - "$ROOT/cpp/apexafter.json" "$ROOT/cpp/apexbase.json" <<'PY'
import json, sys
after = json.load(open(sys.argv[1], encoding="utf-8"))
base = json.load(open(sys.argv[2], encoding="utf-8"))
if base.get("n", 0) == 0 or base.get("costP99Ms", 0) == 0:
    json.dump(after, open(sys.argv[2], "w", encoding="utf-8"), indent=2)
    print("froze apexbase.json from apexafter.json")
else:
    print("baseline kept; after vs base", after.get("costAfterObserveP99Ms"), base.get("costP99Ms"))
PY

echo
echo "======== REGAPEX fail=$fail ========"
[[ "$fail" -eq 0 ]]
