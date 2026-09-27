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
cp "$ROOT/.config/config.example.toml" "$ROOT/.config/config.toml"
# Bind loopback for smoke even if example listens on 0.0.0.0.
sed -i.bak -E "s|^listen = .*|listen = \"${HOST}:${PORT}\"|" "$ROOT/.config/config.toml"
rm -f "$ROOT/.config/config.toml.bak"

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
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"smoke","version":"0"}}}' \
  >/tmp/usearch_mcp_init.json
curl -fsS --max-time 10 -X POST "$BASE/mcp" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}' -o /tmp/usearch_mcp_inited.txt -w '%{http_code}' \
  | tee /tmp/usearch_mcp_inited.code
echo
curl -fsS --max-time 30 -X POST "$BASE/mcp" -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' >/tmp/usearch_mcp.json
# GET SSE must not emit JSON-RPC `data: {...}` stubs (Cursor rejects).
curl -fsS --max-time 5 -X GET "$BASE/mcp" -H 'Accept: text/event-stream' -H 'Authorization: Bearer '"${TOKEN:-}" \
  >/tmp/usearch_mcp_sse.txt || true
if grep -q '^data: {' /tmp/usearch_mcp_sse.txt 2>/dev/null; then
  echo "mcp GET SSE leaked JSON-RPC data frame" >&2
  cat /tmp/usearch_mcp_sse.txt >&2
  exit 1
fi

# At least one upsert + search on the product path (NumKong-on CI).
curl -fsS --max-time 60 -X POST "$BASE/mcp" -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"upsert","arguments":{"id":"smoke-nk","text":"numkong smoke document"}}}' \
  >/tmp/usearch_mcp_upsert.json
curl -fsS --max-time 60 -X POST "$BASE/mcp" -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"search","arguments":{"text":"numkong smoke","k":3}}}' \
  >/tmp/usearch_mcp_search.json

python3 - <<'PY'
import json
emb = json.load(open("/tmp/usearch_emb.json"))
assert emb["data"][0]["embedding"], "empty embedding"
chat = json.load(open("/tmp/usearch_chat.json"))
assert chat["choices"][0]["message"]["content"], "empty chat"
init = json.load(open("/tmp/usearch_mcp_init.json"))
assert init["result"]["protocolVersion"], "initialize failed"
mcp = json.load(open("/tmp/usearch_mcp.json"))
assert len(mcp["result"]["tools"]) >= 1, "no mcp tools"
code = open("/tmp/usearch_mcp_inited.code").read().strip()
assert code == "202", f"notifications/initialized expected 202 got {code}"
up = json.load(open("/tmp/usearch_mcp_upsert.json"))
assert "error" not in up, up
search = json.load(open("/tmp/usearch_mcp_search.json"))
assert "error" not in search, search
print("smoke ok", "dim", len(emb["data"][0]["embedding"]), "tools", len(mcp["result"]["tools"]))
PY

kill "$(cat "${TMPDIR:-/tmp}/usearch_api_smoke.pid")" 2>/dev/null || true
trap - ERR
echo OK
