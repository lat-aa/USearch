#!/usr/bin/env bash
# 本地 smoke 共用：起停 api、鉴权头、pass/bad、是否有 GGUF。
# 用法：source "$(dirname "$0")/smoke_common.sh" 后调用 smoke_boot / smoke_teardown
# 副作用：改写仓库 .config/config.toml（从 example 复制并设 listen/token）。
#
# 就绪条件：带 Bearer 的 GET /ready 返回 ok=true（GGUF 加载完成才会 listen，
# 不能只靠 /alive，否则会在模型仍加载时开跑）。

set -euo pipefail

SMOKE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SMOKE_API_BIN="${API_BIN:-$SMOKE_ROOT/build/api}"
SMOKE_HOST="${HOST:-127.0.0.1}"
SMOKE_PORT="${PORT:-8088}"
SMOKE_BASE="${BASE:-http://${SMOKE_HOST}:${SMOKE_PORT}}"
SMOKE_TOKEN="${TOKEN:-sk-default}"
SMOKE_LOG="${TMPDIR:-/tmp}/usearch_smoke_${SMOKE_PORT}.log"
SMOKE_PID_FILE="${TMPDIR:-/tmp}/usearch_smoke_${SMOKE_PORT}.pid"
# CPU 加载 3B GGUF 可能超过 90s
SMOKE_BOOT_SECS="${SMOKE_BOOT_SECS:-300}"
SMOKE_OK=0
SMOKE_FAIL=0
SMOKE_HAS_GGUF=0

smoke_pass() { echo "PASS  $1"; SMOKE_OK=$((SMOKE_OK + 1)); }
smoke_bad()  { echo "FAIL  $1 — $2"; SMOKE_FAIL=$((SMOKE_FAIL + 1)); }
smoke_skip() { echo "SKIP  $1 — $2"; }

smoke_dump_log() {
  echo "---- api log ($SMOKE_LOG) ----" >&2
  cat "$SMOKE_LOG" >&2 || true
  if [[ -f "${SMOKE_LOG}.err" ]]; then
    echo "---- api stderr (${SMOKE_LOG}.err) ----" >&2
    cat "${SMOKE_LOG}.err" >&2 || true
  fi
}

smoke_kill_pid() {
  local pid="${1:-}"
  [[ -n "$pid" ]] || return 0
  kill "$pid" 2>/dev/null || true
  # Windows：Git Bash 的 kill 有时杀不掉原生 exe
  if command -v taskkill >/dev/null 2>&1; then
    taskkill //F //PID "$pid" >/dev/null 2>&1 || true
  fi
}

SMOKE_PY_BIN=""
smoke_resolve_py() {
  [[ -n "$SMOKE_PY_BIN" ]] && return 0
  local c resolved
  for c in \
    "${SMOKE_PYTHON:-}" \
    "$HOME/.local/bin/python3.12" \
    "$HOME/.local/bin/python3" \
    "/usr/bin/python3" \
    "/c/Users/${USER:-fei}/.local/bin/python3.12.exe" \
    "python3" \
    "python"; do
    [[ -n "$c" ]] || continue
    if [[ "$c" == /* || "$c" == [A-Za-z]:* || "$c" == *".exe" ]]; then
      [[ -x "$c" || -f "$c" ]] || continue
      SMOKE_PY_BIN=$c
      return 0
    fi
    resolved=$(command -v "$c" 2>/dev/null || true)
    [[ -n "$resolved" ]] || continue
    [[ "$resolved" == *"WindowsApps"* ]] && continue
    SMOKE_PY_BIN=$resolved
    return 0
  done
  echo "smoke: no usable Python (set SMOKE_PYTHON=...)" >&2
  return 127
}

smoke_py() {
  smoke_resolve_py || return 127
  "$SMOKE_PY_BIN" "$@"
}

# 覆盖同名函数时必须走已解析二进制，禁止再进 smoke_py→python3 递归。
python3() {
  smoke_resolve_py || return 127
  "$SMOKE_PY_BIN" "$@"
}

smoke_boot() {
  if [[ ! -x "$SMOKE_API_BIN" && ! -f "$SMOKE_API_BIN" ]]; then
    echo "missing api binary: $SMOKE_API_BIN" >&2
    exit 1
  fi
  # Windows 下 ./build/api 常需 .exe
  if [[ ! -f "$SMOKE_API_BIN" && -f "${SMOKE_API_BIN}.exe" ]]; then
    SMOKE_API_BIN="${SMOKE_API_BIN}.exe"
  fi
  # 记为备份：smoke 会覆盖 config.toml，收尾必须还原，避免污染生产配置
  SMOKE_CFG_BAK="${TMPDIR:-/tmp}/usearch_smoke_cfg_${SMOKE_PORT}.toml"
  [ -f "$SMOKE_ROOT/.config/config.toml" ] && cp -f "$SMOKE_ROOT/.config/config.toml" "$SMOKE_CFG_BAK"
  cp "$SMOKE_ROOT/.config/config.example.toml" "$SMOKE_ROOT/.config/config.toml"
  # 绑定 loopback + 固定 Bearer，保证鉴权用例可复现。
  sed -i.bak -E "s|^listen = .*|listen = \"${SMOKE_HOST}:${SMOKE_PORT}\"|" "$SMOKE_ROOT/.config/config.toml"
  sed -i.bak -E "s|^token = .*|token = \"${SMOKE_TOKEN}\"|" "$SMOKE_ROOT/.config/config.toml"
  # SMOKE_HASH=1：强制缺 GGUF→hash 嵌入，避免 /mnt 上加载 2GB 权重拖死 boot。
  if [[ "${SMOKE_HASH:-0}" == "1" ]]; then
    sed -i.bak -E "s|^gguf = .*|gguf = \".config/models/MISSING-smoke.gguf\"|" "$SMOKE_ROOT/.config/config.toml"
  fi
  # 安全隔离：smoke 默认**不碰生产库/索引**（rm index.usearch 会毁掉生产向量！）。
  # 需要真实库的用例显式 SMOKE_SHARE_STORE=1（此时才允许清仓库索引）。
  if [[ "${SMOKE_SHARE_STORE:-0}" = "1" ]]; then
    if [[ -n "${SMOKE_BASE_DB:-}" ]]; then
      sed -i.bak -E "s|^base = .*|base = \"${SMOKE_BASE_DB}\"|" "$SMOKE_ROOT/.config/config.toml"
    fi
    rm -f "$SMOKE_ROOT/.config/index.usearch"
  else
    SMOKE_TMP="${TMPDIR:-/tmp}/usearch_smoke_store_${SMOKE_PORT}"
    mkdir -p "$SMOKE_TMP"
    [[ -n "${SMOKE_BASE_DB:-}" ]] || SMOKE_BASE_DB="$SMOKE_TMP/store.sqlite"
    SMOKE_INDEX="$SMOKE_TMP/index.usearch"
    rm -f "$SMOKE_TMP"/store.sqlite* "$SMOKE_INDEX"
    sed -i.bak -E "s|^base = .*|base = \"$SMOKE_BASE_DB\"|" "$SMOKE_ROOT/.config/config.toml"
    sed -i.bak -E "s|^index = .*|index = \"$SMOKE_INDEX\"|" "$SMOKE_ROOT/.config/config.toml"
  fi
  rm -f "$SMOKE_ROOT/.config/config.toml.bak"

  if [[ -f "$SMOKE_PID_FILE" ]]; then
    smoke_kill_pid "$(cat "$SMOKE_PID_FILE")"
    rm -f "$SMOKE_PID_FILE"
    sleep 1
  fi
  # 释放端口：Linux/WSL 用 fuser；勿在 WSL 里调 Windows powershell 杀端口（易误杀/拖慢）。
  if command -v fuser >/dev/null 2>&1; then
    fuser -k "${SMOKE_PORT}/tcp" 2>/dev/null || true
  elif [[ "$(uname -s 2>/dev/null)" == MINGW* || "$(uname -s 2>/dev/null)" == MSYS* ]] &&
    command -v powershell.exe >/dev/null 2>&1; then
    powershell.exe -NoProfile -Command \
      "Get-NetTCPConnection -LocalPort ${SMOKE_PORT} -State Listen -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id \$_.OwningProcess -Force -ErrorAction SilentlyContinue }" \
      >/dev/null 2>&1 || true
  fi
  sleep 1

  : >"$SMOKE_LOG"
  # Git Bash 下 `exe &` 常在 listen 后被回收；Windows 用 Start-Process 脱离作业。
  local uname_s
  uname_s=$(uname -s 2>/dev/null || echo unknown)
  if [[ "$uname_s" == MINGW* || "$uname_s" == MSYS* || "$uname_s" == CYGWIN* ]] &&
    command -v powershell.exe >/dev/null 2>&1; then
    local win_bin win_root win_log
    win_bin=$(cygpath -w "$SMOKE_API_BIN" 2>/dev/null || echo "$SMOKE_API_BIN")
    win_root=$(cygpath -w "$SMOKE_ROOT" 2>/dev/null || echo "$SMOKE_ROOT")
    win_log=$(cygpath -w "$SMOKE_LOG" 2>/dev/null || echo "$SMOKE_LOG")
    local pid
    # stdout/stderr 不能同时 Redirect 到同一路径；合并到 .err 再 type 拼日志太重，这里只收 stdout，stderr 进旁路文件。
    local win_err="${win_log}.err"
    pid=$(powershell.exe -NoProfile -Command \
      "\$p = Start-Process -FilePath '$win_bin' -ArgumentList 'serve' -WorkingDirectory '$win_root' -RedirectStandardOutput '$win_log' -RedirectStandardError '$win_err' -WindowStyle Hidden -PassThru; Write-Output \$p.Id")
    pid=$(echo "$pid" | tr -d '\r' | tail -n1)
    echo "$pid" >"$SMOKE_PID_FILE"
    # 便于 smoke_dump_log：把 .err 追加提示进主日志头
    echo "# stderr -> $win_err" >>"$SMOKE_LOG"
  else
    (cd "$SMOKE_ROOT" && "$SMOKE_API_BIN" serve) >>"$SMOKE_LOG" 2>&1 &
    echo $! >"$SMOKE_PID_FILE"
  fi
  echo "smoke waiting ready (up to ${SMOKE_BOOT_SECS}s) pid=$(cat "$SMOKE_PID_FILE") ..."

  local ready=""
  for i in $(seq 1 "$SMOKE_BOOT_SECS"); do
    # 必须鉴权 /ready 且 ok：listen 发生在 GGUF 加载之后
    ready=$(curl -fsS --max-time 2 -H "Authorization: Bearer ${SMOKE_TOKEN}" "$SMOKE_BASE/ready" 2>/dev/null || true)
    if echo "$ready" | smoke_py -c "import sys,json;d=json.load(sys.stdin);sys.exit(0 if d.get('ok') is True else 1)" 2>/dev/null; then
      break
    fi
    if grep -Eiq 'missing config|listen.*失败|bind 失败|Fatal' "$SMOKE_LOG" 2>/dev/null; then
      echo "api failed during boot" >&2
      smoke_dump_log
      exit 1
    fi
    # 进程已死
    if [[ -f "$SMOKE_PID_FILE" ]] && ! kill -0 "$(cat "$SMOKE_PID_FILE")" 2>/dev/null; then
      # Windows 上 kill -0 常不可靠；看日志是否已有 listen
      if ! grep -q 'api 监听' "$SMOKE_LOG" 2>/dev/null; then
        sleep 1
        ready=$(curl -fsS --max-time 2 -H "Authorization: Bearer ${SMOKE_TOKEN}" "$SMOKE_BASE/ready" 2>/dev/null || true)
        if echo "$ready" | smoke_py -c "import sys,json;d=json.load(sys.stdin);sys.exit(0 if d.get('ok') is True else 1)" 2>/dev/null; then
          break
        fi
        echo "api process exited before ready" >&2
        smoke_dump_log
        exit 1
      fi
    fi
    sleep 1
    if [[ "$i" -eq "$SMOKE_BOOT_SECS" ]]; then
      echo "api did not become ready in ${SMOKE_BOOT_SECS}s" >&2
      smoke_dump_log
      exit 1
    fi
  done

  if echo "$ready" | smoke_py -c "import sys,json;d=json.load(sys.stdin);sys.exit(0 if d.get('model') is True else 1)" 2>/dev/null; then
    SMOKE_HAS_GGUF=1
  else
    SMOKE_HAS_GGUF=0
  fi
  echo "smoke base=$SMOKE_BASE token=set gguf=$SMOKE_HAS_GGUF ready=$ready"
}

smoke_teardown() {
  # 还原被 smoke 覆盖的 config.toml
  if [[ -n "${SMOKE_CFG_BAK:-}" && -f "$SMOKE_CFG_BAK" ]]; then
    cp -f "$SMOKE_CFG_BAK" "$SMOKE_ROOT/.config/config.toml"
  fi
  if [[ -f "$SMOKE_PID_FILE" ]]; then
    smoke_kill_pid "$(cat "$SMOKE_PID_FILE")"
    rm -f "$SMOKE_PID_FILE"
  fi
  if command -v fuser >/dev/null 2>&1; then
    fuser -k "${SMOKE_PORT}/tcp" 2>/dev/null || true
  elif [[ "$(uname -s 2>/dev/null)" == MINGW* || "$(uname -s 2>/dev/null)" == MSYS* ]] &&
    command -v powershell.exe >/dev/null 2>&1; then
    powershell.exe -NoProfile -Command \
      "Get-NetTCPConnection -LocalPort ${SMOKE_PORT} -State Listen -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id \$_.OwningProcess -Force -ErrorAction SilentlyContinue }" \
      >/dev/null 2>&1 || true
  fi
}

smoke_summary() {
  echo
  echo "======== SUMMARY pass=$SMOKE_OK fail=$SMOKE_FAIL ========"
  [[ "$SMOKE_FAIL" -eq 0 ]]
}

# AUTH / JSON / MCP 数组供 curl 展开
smoke_auth_headers() {
  AUTH=(-H "Authorization: Bearer ${SMOKE_TOKEN}")
  JSON=(-H "Content-Type: application/json" -H "Accept: application/json")
  MCP=(-H "Authorization: Bearer ${SMOKE_TOKEN}" -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream")
}
