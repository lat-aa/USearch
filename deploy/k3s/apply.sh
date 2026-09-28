#!/bin/sh
# Apply deploy/k3s/api.yaml against local Ubuntu WSL k3s (glibc + ubuntu:26.04).
#
# Usage:
#   deploy/k3s/apply.sh            # 常规 apply（含模型就位检查）
#   deploy/k3s/apply.sh redeploy   # 重建二进制并滚动重启 api Pod
set -e
# Prefer repo path (script may be copied/piped to /tmp).
if [ -f /mnt/e/data/USearch/CMakeLists.txt ]; then
  ROOT=/mnt/e/data/USearch
else
  ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
fi
cd "$ROOT"
export KUBECONFIG="${KUBECONFIG:-/etc/rancher/k3s/k3s.yaml}"

# ---- redeploy 子命令：拷贝最新 api 二进制并滚动重启 Pod ----
if [ "${1:-}" = "redeploy" ]; then
  found=0
  for src in "$HOME/usearch-build/api" /root/usearch-build/api; do
    if [ -x "$src" ]; then
      cp -f "$src" .local/api
      chmod +x .local/api
      echo "copied $src -> .local/api"
      found=1
      break
    fi
  done
  if [ "$found" = 0 ]; then
    echo "error: no build binary found; run deploy/k3s/_rebuild.sh first" >&2
    exit 1
  fi
  k3s kubectl -n usearch delete pod -l app=api --force --grace-period=0 || true
  i=0
  while [ "$i" -lt 90 ]; do
    ready=$(k3s kubectl -n usearch get pods -l app=api --no-headers 2>/dev/null | awk '{print $2}')
    echo "ready=$ready"
    case "$ready" in 1/1) break ;; esac
    i=$((i + 1))
    sleep 2
  done
  k3s kubectl -n usearch get pods,endpoints -o wide
  curl -fsS -m 5 http://127.0.0.1:30088/alive && echo " (nodePort OK)" || echo "nodePort /alive FAIL"
  curl -fsS -m 5 http://api.ya.com/alive && echo " (ingress OK)" || echo "ingress /alive FAIL"
  exit 0
fi

if [ ! -f .config/config.toml ]; then
  cp .config/config.example.toml .config/config.toml
fi

if grep -Eq '^listen[[:space:]]*=[[:space:]]*"127\.0\.0\.1:' .config/config.toml; then
  echo "error: set listen to 0.0.0.0:8088 in .config/config.toml for Ingress/NodePort"
  exit 1
fi

# Align with Cursor MCP Authorization: Bearer sk-default
if grep -Eq '^token[[:space:]]*=[[:space:]]*""' .config/config.toml; then
  sed -i.bak -E 's|^token[[:space:]]*=[[:space:]]*"".*|token = "sk-default"  # Bearer; MCP Authorization|' .config/config.toml
  rm -f .config/config.toml.bak
  echo "set token=sk-default in .config/config.toml"
fi

mkdir -p .local
if [ ! -x .local/api ]; then
  if [ -x "$HOME/usearch-build/api" ]; then
    cp -f "$HOME/usearch-build/api" .local/api
    chmod +x .local/api
  elif [ -x /root/usearch-build/api ]; then
    cp -f /root/usearch-build/api .local/api
    chmod +x .local/api
  else
    echo "missing .local/api — on Ubuntu: cmake --build ~/usearch-build --target api && cp ~/usearch-build/api .local/api"
    exit 1
  fi
fi
# Reject leftover Alpine musl binary (needs Ubuntu glibc build).
if file .local/api 2>/dev/null | grep -q musl; then
  echo "error: .local/api is musl (Alpine); rebuild on Ubuntu and overwrite .local/api"
  exit 1
fi
if [ ! -d .local/lib ] || [ -z "$(ls -A .local/lib 2>/dev/null)" ]; then
  echo "missing .local/lib — on Ubuntu:"
  echo "  mkdir -p .local/lib && cp /usr/lib/x86_64-linux-gnu/libstdc++.so.6 /usr/lib/x86_64-linux-gnu/libgomp.so.1 /usr/lib/x86_64-linux-gnu/libgcc_s.so.1 /usr/lib/x86_64-linux-gnu/libssl.so.3 /usr/lib/x86_64-linux-gnu/libcrypto.so.3 .local/lib/"
  exit 1
fi

# ---- 模型就位检查（~2.57GB GGUF 必须已落盘到 Linux ext4） ----
MODEL_DIR=/var/lib/usearch/models
MODEL_FILE="$MODEL_DIR/Nanbeige4.2-3B-Q4_K_M.gguf"
if [ ! -d "$MODEL_DIR" ] || [ ! -f "$MODEL_FILE" ]; then
  echo "error: model missing: $MODEL_FILE" >&2
  echo "  run: deploy/k3s/fetch-model.sh" >&2
  exit 1
fi

if ! k3s ctr images ls 2>/dev/null | grep -q 'docker.io/library/ubuntu:26.04'; then
  k3s ctr images pull docker.m.daocloud.io/library/ubuntu:26.04
  k3s ctr images tag docker.m.daocloud.io/library/ubuntu:26.04 docker.io/library/ubuntu:26.04
fi
k3s kubectl apply -f deploy/k3s/api.yaml
k3s kubectl -n usearch rollout status deploy/api --timeout=900s
k3s kubectl -n usearch get pods,svc,ingress -o wide
echo ""
echo "hosts (Windows + WSL):  192.168.164.162 api.ya.com"
echo "  WSL IP 漂移后需更新 hosts（Ubuntu 内用: hostname -I 查看当前 IP）"
echo "Ingress:  http://api.ya.com/alive"
echo "          http://api.ya.com/v1   (Authorization: Bearer sk-default)"
echo "          http://api.ya.com/mcp  (Authorization: Bearer sk-default)"
echo "NodePort: http://127.0.0.1:30088/v1  MCP http://127.0.0.1:30088/mcp"