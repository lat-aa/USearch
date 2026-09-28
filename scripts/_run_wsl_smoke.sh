#!/usr/bin/env bash
set -euo pipefail
ROOT=/mnt/e/data/USearch
DST=$HOME/usearch-smoke
pkill -f "$DST/.local/api" 2>/dev/null || true
pkill -f "$ROOT/.local/api" 2>/dev/null || true
sleep 1
mkdir -p "$DST/.local" "$DST/scripts" "$DST/.config/models"
cp -a /home/fei/usearch-build/api "$DST/.local/api"
cp -a /home/fei/usearch-build/api "$ROOT/.local/api" 2>/dev/null || \
  { cp -a /home/fei/usearch-build/api "$ROOT/.local/api.new" && mv -f "$ROOT/.local/api.new" "$ROOT/.local/api"; }

# 刷新脚本与规则
cp -a "$ROOT/scripts/smoke_common.sh" "$ROOT/scripts/smoke_mcp.sh" \
  "$ROOT/scripts/smoke_v1.sh" "$ROOT/scripts/smoke_apex.sh" \
  "$ROOT/scripts/smoke_nightly.sh" "$ROOT/scripts/stats.sh" \
  "$ROOT/scripts/smoke_hooks.js" "$ROOT/scripts/presync.js" \
  "$ROOT/scripts/injectmodel.js" "$DST/scripts/"
cp -a "$ROOT/.config/config.example.toml" "$DST/.config/"
cp -a "$ROOT/.config/rules" "$DST/.config/"
cp -a "$ROOT/.config/decide" "$DST/.config/" 2>/dev/null || true
sed -i 's/\r$//' "$DST"/scripts/*.sh "$DST"/scripts/*.js
chmod +x "$DST/.local/api" "$DST"/scripts/*.sh

cd "$DST"
export API_BIN="$DST/.local/api"
export LD_LIBRARY_PATH="$DST/.local/lib:${LD_LIBRARY_PATH:-}"
export TOKEN=sk-default
export SMOKE_HASH=1
export SMOKE_BOOT_SECS=60

echo "==== tools probe ===="
rm -f .config/index.usearch
cp .config/config.example.toml .config/config.toml
sed -i -E 's|^listen = .*|listen = "127.0.0.1:18999"|' .config/config.toml
sed -i -E 's|^token = .*|token = "sk-default"|' .config/config.toml
sed -i -E 's|^gguf = .*|gguf = ".config/models/MISSING-smoke.gguf"|' .config/config.toml
"$API_BIN" serve >/tmp/tools_probe.log 2>&1 &
pid=$!
for _ in $(seq 1 30); do
  curl -fsS -m 1 -H "Authorization: Bearer sk-default" http://127.0.0.1:18999/ready && break
  sleep 0.3
done
curl -fsS -m 10 -H "Authorization: Bearer sk-default" -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' http://127.0.0.1:18999/mcp \
  | python3 -c "import sys,json; print(sorted(t['name'] for t in json.load(sys.stdin)['result']['tools']))"
kill $pid 2>/dev/null || true
wait $pid 2>/dev/null || true

echo "==== smoke_mcp ===="
PORT=18091 ./scripts/smoke_mcp.sh
echo "==== smoke_v1 ===="
PORT=18092 ./scripts/smoke_v1.sh
echo "==== smoke_apex ===="
PORT=18093 ./scripts/smoke_apex.sh
if command -v node >/dev/null 2>&1; then
  echo "==== smoke_hooks ===="
  node scripts/smoke_hooks.js
else
  # 用 Windows node 经 /mnt 也可；此处跳过
  echo "SKIP smoke_hooks (no node); run on host: node scripts/smoke_hooks.js"
fi
echo "==== ALL LOCAL SMOKE OK ===="
