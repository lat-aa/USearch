#!/bin/bash
# 本地/网关 gate+observe 冒烟（需 api 已监听且 token 对齐）
# Usage: BASE=http://127.0.0.1:8088 TOKEN=sk-default bash scripts/smoke_gate.sh
set -euo pipefail
BASE="${BASE:-http://api.ya.com}"
TOKEN="${TOKEN:-sk-default}"
AUTH=(-H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json")

echo "== gate =="
curl -fsS --max-time 120 "${AUTH[@]}" -d '{"task":"ping gate"}' "$BASE/v1/gate" | tee /tmp/gate.json
python3 -c "import json;d=json.load(open('/tmp/gate.json'));assert d['status'] in ('answered','pack','refuse');assert 'policyFingerprint' in d"

echo "== observe =="
curl -fsS --max-time 30 "${AUTH[@]}" -H "Accept: application/json, text/event-stream" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"observe","arguments":{"title":"smoke","summary":"gate smoke","outcome":"ok"}}}' \
  "$BASE/mcp" | tee /tmp/obs.json
python3 -c "import json;r=json.load(open('/tmp/obs.json'));t=json.loads(r['result']['content'][0]['text']);assert t['ok'] and t['status']=='pending'"

echo "OK smoke_gate"
