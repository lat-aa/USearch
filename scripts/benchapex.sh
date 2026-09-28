#!/usr/bin/env bash
# apex 延迟 KPI：presync / cost / observe→cost / chat∥distill 对抗。
# 用法：API_BIN=./build/api TOKEN=sk-default ./scripts/benchapex.sh
# 写出 cpp/apexafter.json；相对 cpp/apexbase.json 过门禁（基线为 0 时用绝对阈值）。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=scripts/smoke_common.sh
source "$ROOT/scripts/smoke_common.sh"

trap 'smoke_dump_log; smoke_teardown' ERR
smoke_boot
smoke_auth_headers
trap 'smoke_teardown' EXIT

OUT="$ROOT/cpp/apexafter.json"
BASELINE="$ROOT/cpp/apexbase.json"
N="${BENCH_N:-20}"

time_ms() {
  local url="$1" data="${2:-}"
  python3 - "$SMOKE_BASE$url" "$SMOKE_TOKEN" "$data" <<'PY'
import sys, time, urllib.request
url, token, data = sys.argv[1], sys.argv[2], sys.argv[3]
req = urllib.request.Request(url, data=data.encode() if data else None, method="POST" if data else "GET")
req.add_header("Authorization", "Bearer " + token)
if data:
    req.add_header("Content-Type", "application/json")
    req.add_header("Accept", "application/json")
t0 = time.perf_counter()
try:
    with urllib.request.urlopen(req, timeout=60) as r:
        r.read()
except Exception:
    pass
print((time.perf_counter() - t0) * 1000.0)
PY
}

metrics_json() {
  curl -fsS --max-time 8 "${AUTH[@]}" "$SMOKE_BASE/v1/metrics"
}

p99() {
  python3 -c 'import sys; xs=sorted(float(x) for x in sys.argv[1:]);
k=max(0,min(len(xs)-1,int(round((len(xs)-1)*0.99)))) if xs else 0; print(xs[k] if xs else 0)' "$@"
}

echo "== warm: rules/cost 一次（descVec 已在启动缓存）=="
curl -fsS --max-time 60 "${MCP[@]}" \
  -d '{"jsonrpc":"2.0","id":0,"method":"tools/call","params":{"name":"cost","arguments":{"task":"warm","actual_model":"composer-2"}}}' \
  "$SMOKE_BASE/mcp" >/dev/null

presync=()
cost=()
observe=()
costAfterObs=()
for i in $(seq 1 "$N"); do
  presync+=("$(time_ms /v1/presync '{"task":"bench presync l1","files":["a.cpp"]}')")
  cost+=("$(time_ms /mcp '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"cost","arguments":{"task":"bench cost","actual_model":"composer-2"}}}')")
  observe+=("$(time_ms /mcp '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"observe","arguments":{"title":"bench","summary":"x","outcome":"ok"}}}')")
  # 不变量 1：observe 后立刻 cost，不得等蒸馏
  costAfterObs+=("$(time_ms /mcp '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"cost","arguments":{"task":"after observe","actual_model":"composer-2"}}}')")
done

# chat∥distill：灌 queue 后持续打 presync，读 stealChat / lockWait
echo "== antagonism: observe flood then presync under load =="
for i in $(seq 1 8); do
  curl -fsS --max-time 15 "${MCP[@]}" \
    -d "{\"jsonrpc\":\"2.0\",\"id\":$i,\"method\":\"tools/call\",\"params\":{\"name\":\"observe\",\"arguments\":{\"title\":\"ant$i\",\"summary\":\"distill load $i\",\"outcome\":\"ok\"}}}" \
    "$SMOKE_BASE/mcp" >/dev/null || true
done
steal0=$(metrics_json | python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('encode',{}).get('stealChat',0))")
antPresync=()
for i in $(seq 1 10); do
  antPresync+=("$(time_ms /v1/presync '{"task":"antagonism presync","files":["x.cpp"]}')")
done
m1=$(metrics_json)
steal1=$(echo "$m1" | python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('encode',{}).get('stealChat',0))")
lockMax=$(echo "$m1" | python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('encode',{}).get('lockWaitMaxMs',0))")
chatBusy=$(echo "$m1" | python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('encode',{}).get('chatBusy',0))")

presyncP99=$(p99 "${presync[@]}")
costP99=$(p99 "${cost[@]}")
observeP99=$(p99 "${observe[@]}")
costAfterP99=$(p99 "${costAfterObs[@]}")
antPresyncP99=$(p99 "${antPresync[@]}")

python3 - "$OUT" "$presyncP99" "$costP99" "$observeP99" "$costAfterP99" "$antPresyncP99" "$steal0" "$steal1" "$lockMax" "$chatBusy" "$N" <<'PY'
import json, sys
path = sys.argv[1]
out = {
    "presyncP99Ms": float(sys.argv[2]),
    "costP99Ms": float(sys.argv[3]),
    "observeP99Ms": float(sys.argv[4]),
    "costAfterObserveP99Ms": float(sys.argv[5]),
    "antPresyncP99Ms": float(sys.argv[6]),
    "stealChatBefore": int(float(sys.argv[7])),
    "stealChatAfter": int(float(sys.argv[8])),
    "lockWaitMaxMs": float(sys.argv[9]),
    "chatBusy": int(float(sys.argv[10])),
    "n": int(sys.argv[11]),
}
json.dump(out, open(path, "w", encoding="utf-8"), indent=2)
print("wrote", path)
print(json.dumps(out, indent=2))
PY

echo "$m1" | python3 -c "
import sys,json
d=json.load(sys.stdin)
assert 'encode' in d and 'chatBusy' in d['encode']
assert 'edge' in d and 'async' in d
print('metrics ok stealChat', d['encode'].get('stealChat'), 'chatBusy', d['encode'].get('chatBusy'))
" && smoke_pass "metrics schema" || smoke_bad "metrics" "$m1"

# 门禁：绝对阈值（方案 KPI）；基线非 0 时另要求 ≤ 基线/3
python3 - "$OUT" "$BASELINE" <<'PY'
import json, sys
after = json.load(open(sys.argv[1], encoding="utf-8"))
base = json.load(open(sys.argv[2], encoding="utf-8"))
fails = []

def gate(name, got, abs_max, base_key=None):
    if got > abs_max:
        fails.append(f"{name}={got:.1f}ms > {abs_max}ms")
        return
    if base_key and base.get(base_key, 0) > 0:
        lim = base[base_key] / 3.0
        if got > lim:
            fails.append(f"{name}={got:.1f}ms > baseline/3={lim:.1f}ms")

# 不变量 1：observe 后 cost 必灭 ~75s 级卡顿
gate("costAfterObserveP99", after["costAfterObserveP99Ms"], 40.0, "costP99Ms")
gate("costP99", after["costP99Ms"], 40.0, "costP99Ms")
gate("presyncP99", after["presyncP99Ms"], 30.0, "presyncP99Ms")
gate("antPresyncP99", after["antPresyncP99Ms"], 50.0)
gate("observeP99", after["observeP99Ms"], 40.0)
# 用户活跃时 Worker try_lock 失败计入 stealChat 是让路成功；lockWaitMax 不得爆炸到秒级
if after["lockWaitMaxMs"] > 5000:
    fails.append(f"lockWaitMaxMs={after['lockWaitMaxMs']} > 5000")
if fails:
    print("KPI FAIL:")
    for f in fails:
        print(" ", f)
    raise SystemExit(1)
print("KPI PASS")
PY
smoke_pass "KPI gates" || smoke_bad "KPI" "fail"

smoke_summary
