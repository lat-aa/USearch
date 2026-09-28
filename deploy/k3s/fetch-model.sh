#!/bin/sh
# 下载/复制 Nanbeige GGUF 到 Linux ext4 盘（/var/lib/usearch/models），并做 sha256 校验。
# 2.44GB 模型放 drvfs(/mnt/e) 会因 9p 随机读慢而拖长启动窗口，务必落盘到 /var/lib。
#
# Usage:
#   deploy/k3s/fetch-model.sh                          # 默认 HF 源
#   MODEL_SRC=/mnt/e/models/Nanbeige4.1-3B-Instruct.Q4_K_M.gguf ./fetch-model.sh   # 本地复制
#   MODEL_SRC=https://example.com/model.gguf ./fetch-model.sh                       # URL 下载
set -e

MODEL_DIR="${MODEL_DIR:-/var/lib/usearch/models}"
MODEL_NAME="Nanbeige4.1-3B-Instruct.Q4_K_M.gguf"
DEST="$MODEL_DIR/$MODEL_NAME"
EXPECT="043246350c952877b38958a9e35c480419008b6b2d52bedaf2b805ed2447b4df"
# HF 仓库 Edge-Quant/Nanbeige4.1-3B-Q4_K_M-GGUF；确切文件名以仓库实际为准，可用 MODEL_SRC 覆盖。
MODEL_SRC="${MODEL_SRC:-https://huggingface.co/Edge-Quant/Nanbeige4.1-3B-Q4_K_M-GGUF/resolve/main/Nanbeige4.1-3B-Instruct.Q4_K_M.gguf}"

sha_of() { sha256sum "$1" | awk '{print $1}'; }

mkdir -p "$MODEL_DIR"

# 幂等：目标已存在且 hash 匹配 → 直接跳过
if [ -f "$DEST" ] && [ "$(sha_of "$DEST")" = "$EXPECT" ]; then
  echo "OK: model already present and verified: $DEST"
  exit 0
fi

TMP="$DEST.download.$$"
trap 'rm -f "$TMP"' EXIT INT TERM

case "$MODEL_SRC" in
  http://*|https://*)
    echo "downloading: $MODEL_SRC"
    curl -fL --retry 3 --retry-delay 2 --connect-timeout 15 -o "$TMP" "$MODEL_SRC"
    ;;
  *)
    if [ -f "$MODEL_SRC" ]; then
      echo "copying: $MODEL_SRC"
      cp -f "$MODEL_SRC" "$TMP"
    else
      echo "error: MODEL_SRC is neither a URL nor an existing file: $MODEL_SRC" >&2
      exit 1
    fi
    ;;
esac

echo "verifying sha256 ..."
if [ "$(sha_of "$TMP")" != "$EXPECT" ]; then
  echo "error: sha256 mismatch for $MODEL_SRC" >&2
  echo "  expected $EXPECT" >&2
  echo "  got      $(sha_of "$TMP")" >&2
  exit 1
fi

mv -f "$TMP" "$DEST"
chmod 644 "$DEST"
echo "OK: model installed: $DEST"