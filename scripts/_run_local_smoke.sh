#!/usr/bin/env bash
# 本机一键跑 L0+L1+L2（WSL Linux api）。
# Windows api.exe 当前会在 listen 后 0xC0000409 崩溃，故默认用 .local/api。
# 默认 SMOKE_HASH=1 走 hash 嵌入以在合理时间内验通脚本；要真 GGUF 设 SMOKE_HASH=0。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# 去掉 CRLF（在 /mnt/e 上编辑的脚本常见）
sed -i 's/\r$//' scripts/smoke_common.sh scripts/smoke_mcp.sh scripts/smoke_v1.sh \
  scripts/smoke_apex.sh scripts/smoke_nightly.sh scripts/stats.sh 2>/dev/null || true

export TOKEN="${TOKEN:-sk-default}"
export SMOKE_BOOT_SECS="${SMOKE_BOOT_SECS:-120}"
export SMOKE_HASH="${SMOKE_HASH:-1}"

if [[ -z "${API_BIN:-}" ]]; then
  if [[ -x "$ROOT/.local/api" ]]; then
    export API_BIN="$ROOT/.local/api"
    export LD_LIBRARY_PATH="${ROOT}/.local/lib:${LD_LIBRARY_PATH:-}"
  elif [[ -x "$ROOT/build/api" ]]; then
    export API_BIN="$ROOT/build/api"
  else
    echo "missing Linux api binary (.local/api or build/api)" >&2
    exit 1
  fi
fi

echo "API_BIN=$API_BIN SMOKE_HASH=$SMOKE_HASH"
python3 -c "print('py-ok')"

for p in 18091 18092 18093; do
  fuser -k "${p}/tcp" 2>/dev/null || true
done
pkill -f "$API_BIN" 2>/dev/null || true
sleep 1

echo "==== smoke_mcp L0 port 18091 ===="
PORT=18091 ./scripts/smoke_mcp.sh
echo "==== smoke_v1 L1 port 18092 ===="
PORT=18092 ./scripts/smoke_v1.sh
echo "==== smoke_apex L2 port 18093 ===="
PORT=18093 ./scripts/smoke_apex.sh
echo "==== test_hooks L3 ===="
if command -v node >/dev/null 2>&1; then
  node scripts/test_hooks.js
else
  echo "SKIP test_hooks (no node in WSL)"
fi
echo "==== ALL LOCAL SMOKE OK ===="
