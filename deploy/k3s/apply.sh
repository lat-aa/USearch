#!/bin/sh
# Apply deploy/k3s/api.yaml against local k3s (prefer Ubuntu WSL if Alpine:6443 conflicts).
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
export KUBECONFIG="${KUBECONFIG:-/etc/rancher/k3s/k3s.yaml}"

if [ ! -f .config/config.toml ]; then
  cp .config/config.example.toml .config/config.toml
fi

if grep -Eq '^listen[[:space:]]*=[[:space:]]*"127\.0\.0\.1:' .config/config.toml; then
  echo "error: set listen to 0.0.0.0:8088 in .config/config.toml for NodePort"
  exit 1
fi

mkdir -p .local
if [ ! -x .local/api ]; then
  if [ -x /root/usearch-build/api ]; then
    cp -f /root/usearch-build/api .local/api
    chmod +x .local/api
  else
    echo "missing .local/api — build api then: cp <build>/api .local/api"
    exit 1
  fi
fi
if [ ! -d .local/lib ] || [ -z "$(ls -A .local/lib 2>/dev/null)" ]; then
  echo "missing .local/lib (libstdc++ libgomp libssl …) — on Alpine:"
  echo "  mkdir -p .local/lib && cp /usr/lib/libstdc++.so.6 /usr/lib/libgomp.so.1 /usr/lib/libgcc_s.so.1 /usr/lib/libssl.so.3 /usr/lib/libcrypto.so.3 .local/lib/"
  exit 1
fi

if ! k3s ctr images ls 2>/dev/null | grep -q 'docker.io/library/alpine:3.20'; then
  k3s ctr images pull docker.m.daocloud.io/library/alpine:3.20
  k3s ctr images tag docker.m.daocloud.io/library/alpine:3.20 docker.io/library/alpine:3.20
fi
k3s kubectl apply -f deploy/k3s/api.yaml
k3s kubectl -n usearch rollout status deploy/api --timeout=900s
k3s kubectl -n usearch get pods,svc -o wide
echo "NodePort http://127.0.0.1:30088/v1  MCP http://127.0.0.1:30088/mcp"
