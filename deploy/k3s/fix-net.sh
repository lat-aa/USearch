#!/bin/sh
# Repair WSL Ubuntu k3s networking so Traefik Ingress (api.ya.com:80) works.
# Root cause on Ubuntu 26.04+: iptables-nft + orphaned CNI veths → pods cannot reach
# ClusterIP / kubelet cannot probe → Traefik CrashLoop.
set -e
export KUBECONFIG="${KUBECONFIG:-/etc/rancher/k3s/k3s.yaml}"

if [ "$(id -u)" -ne 0 ]; then
  echo "error: run as root (sudo)"
  exit 1
fi

echo "== iptables → legacy =="
update-alternatives --set iptables /usr/sbin/iptables-legacy
update-alternatives --set ip6tables /usr/sbin/ip6tables-legacy
iptables -V

echo "== recreate not-ready / stale kube-system pods =="
k3s kubectl -n kube-system delete pod -l app.kubernetes.io/name=traefik --force --grace-period=0 2>/dev/null || true
k3s kubectl -n kube-system delete pod -l k8s-app=kube-dns --force --grace-period=0 2>/dev/null || true
k3s kubectl -n kube-system delete pod -l k8s-app=metrics-server --force --grace-period=0 2>/dev/null || true
k3s kubectl -n kube-system delete pod -l 'svccontroller.k3s.cattle.io/svcname=traefik' --force --grace-period=0 2>/dev/null || true
k3s kubectl -n kube-system delete pod -l app=local-path-provisioner --force --grace-period=0 2>/dev/null || true
for p in $(k3s kubectl -n kube-system get pods --no-headers 2>/dev/null | awk '$2 ~ /0\// {print $1}'); do
  k3s kubectl -n kube-system delete pod "$p" --force --grace-period=0 || true
done

echo "== restart k3s =="
systemctl restart k3s
i=0
while [ "$i" -lt 60 ]; do
  if k3s kubectl get --raw=/readyz >/dev/null 2>&1; then
    break
  fi
  i=$((i + 1))
  sleep 2
done

k3s kubectl -n kube-system rollout status deploy/coredns --timeout=180s
k3s kubectl -n kube-system rollout status deploy/traefik --timeout=180s
k3s kubectl -n kube-system get pods -o wide

EXT=$(k3s kubectl -n kube-system get svc traefik -o jsonpath='{.status.loadBalancer.ingress[0].ip}' 2>/dev/null || true)
echo
echo "Traefik LB: ${EXT:-unknown}"
echo "hosts: ${EXT:-<wsl-ip>} api.ya.com"
if [ -n "$EXT" ]; then
  curl -fsS -m 5 -H 'Host: api.ya.com' "http://${EXT}/alive" && echo
fi
echo FIX_NET_OK
