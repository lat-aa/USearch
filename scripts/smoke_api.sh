#!/usr/bin/env bash
# Portable HTTP/MCP smoke for the api binary (hash embed if GGUF missing).
# Usage: API_BIN=./build/api ./scripts/smoke_api.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
API_BIN="${API_BIN:-$ROOT/build/api}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-8088}"
BASE="http://${HOST}:${PORT}"
LOG="${TMPDIR:-/tmp}/usearch_api_smoke.log"

dump_log() {
  echo "---- api log ----" >&2
  cat "$LOG" >&2 || true
}
trap 'dump_log' ERR

if [[ ! -x "$API_BIN" && ! -f "$API_BIN" ]]; then
  echo "missing api binary: $API_BIN" >&2
  exit 1
fi
# Always start from example so CI is deterministic.
cp "$ROOT/.config/config.example.json" "$ROOT/.config/config.json"
# Bind loopback for smoke even if example listens on 0.0.0.0.
python3 - <<PY
import json
from pathlib import Path
p = Path("${ROOT}/.config/config.json")
cfg = json.loads(p.read_text())
cfg["listen"] = "${HOST}:${PORT}"
p.write_text(json.dumps(cfg, indent=2) + "\n")
PY

pkill -f "$API_BIN" 2>/dev/null || true
sleep 1
(cd "$ROOT" && "$API_BIN" serve) >"$LOG" 2>&1 &
echo $! >"${TMPDIR:-/tmp}/usearch_api_smoke.pid"

for i in $(seq 1 90); do
  if curl -fsS --max-time 2 "$BASE/alive" >/dev/null 2>&1; then
    break
  fi
  if grep -Eiq 'missing config|listen.*失败|failed to|Fatal' "$LOG" 2>/dev/null; then
    exit 1
  fi
  sleep 1
  if [[ "$i" -eq 90 ]]; then
    echo "api did not become ready" >&2
    exit 1
  fi
done

curl -fsS --max-time 10 "$BASE/ready" | tee /tmp/usearch_ready.json
curl -fsS --max-time 30 "$BASE/v1/embeddings" -H 'Content-Type: application/json' -d '{"input":"hello"}' >/tmp/usearch_emb.json
curl -fsS --max-time 60 "$BASE/v1/chat/completions" -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"ping"}]}' >/tmp/usearch_chat.json
curl -fsS --max-time 30 -X POST "$BASE/mcp" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' >/tmp/usearch_mcp.json

python3 - <<'PY'
import json
emb = json.load(open("/tmp/usearch_emb.json"))
assert emb["data"][0]["embedding"], "empty embedding"
chat = json.load(open("/tmp/usearch_chat.json"))
assert chat["choices"][0]["message"]["content"], "empty chat"
mcp = json.load(open("/tmp/usearch_mcp.json"))
assert len(mcp["result"]["tools"]) >= 1, "no mcp tools"
print("smoke ok", "dim", len(emb["data"][0]["embedding"]), "tools", len(mcp["result"]["tools"]))
PY

kill "$(cat "${TMPDIR:-/tmp}/usearch_api_smoke.pid")" 2>/dev/null || true
trap - ERR
echo OK
