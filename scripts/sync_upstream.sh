#!/usr/bin/env bash
# 同步上游 unum-cloud/USearch 的 include/usearch（A 轨）。
# 本 fork 允许在 include/usearch 有本地补丁（如 OOM 回滚）；同步时若冲突需手工解决。
#
# B 轨（永不被本脚本覆盖）：tools/apex/、tools/sqlite/、tests/apex/、.config/、.cursor/、.codex/、scripts/smoke_*、deploy/
#
# 用法：
#   ./scripts/sync_upstream.sh           # fetch + checkout upstream headers
#   ./scripts/sync_upstream.sh --check   # 仅 diff，不改工作区
set -euo pipefail
ROOT="$(cd -- "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

REMOTE="${UPSTREAM_REMOTE:-upstream}"
BRANCH="${UPSTREAM_BRANCH:-main}"

if ! git remote get-url "$REMOTE" >/dev/null 2>&1; then
  echo "adding remote $REMOTE -> https://github.com/unum-cloud/USearch.git"
  git remote add "$REMOTE" https://github.com/unum-cloud/USearch.git
fi

git fetch "$REMOTE" "$BRANCH"

if [[ "${1:-}" == "--check" ]]; then
  echo "== diff vs $REMOTE/$BRANCH:include/usearch =="
  git diff --stat "$REMOTE/$BRANCH" -- include/usearch || true
  exit 0
fi

# 检出上游头文件；本地补丁冲突时 git 会保留冲突标记，勿 --force。
git checkout "$REMOTE/$BRANCH" -- include/usearch
echo "Synced include/usearch from $REMOTE/$BRANCH"
echo "Review local A-track patches (OOM etc.) before commit:"
echo "  git diff -- include/usearch"
echo "B-track paths were not touched."
