#!/bin/sh
# Apply deploy/k3s/api.yaml against local Ubuntu WSL k3s (glibc + ubuntu:26.04).
set -e
# Prefer repo path (script may be copied/piped to /tmp).
if [ -f /mnt/e/data/USearch/CMakeLists.txt ]; then
  ROOT=/mnt/e/data/USearch
else
  ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
fi
cd "$ROOT"
export KUBECONFIG="${KUBECONFIG:-/etc/rancher/k3s/k3s.yaml}"

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

if ! k3s ctr images ls 2>/dev/null | grep -q 'docker.io/library/ubuntu:26.04'; then
  k3s ctr images pull docker.m.daocloud.io/library/ubuntu:26.04
  k3s ctr images tag docker.m.daocloud.io/library/ubuntu:26.04 docker.io/library/ubuntu:26.04
fi
k3s kubectl apply -f deploy/k3s/api.yaml
k3s kubectl -n usearch rollout status deploy/api --timeout=900s
k3s kubectl -n usearch get pods,svc,ingress -o wide
echo ""
echo "hosts (Windows + WSL):  127.0.0.1 api.ya.com"
echo "Ingress:  http://api.ya.com/alive"
echo "          http://api.ya.com/v1   (Authorization: Bearer sk-default)"
echo "          http://api.ya.com/mcp  (Authorization: Bearer sk-default)"
echo "NodePort: http://127.0.0.1:30088/v1  MCP http://127.0.0.1:30088/mcp"
