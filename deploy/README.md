# USearch `deploy/` 部署运行手册

本目录是 **USearch api 网关** 的唯一权威部署路径。单节点 k3s 只跑在 **Ubuntu WSL**
（systemd 管理）；Alpine 已不再承载 k3s（副本见 `E:\do\Alpine`，仅作备份）。

## 拓扑

```
Windows 宿主 (APEXLY)
└── Ubuntu WSL (systemd)
    └── k3s 单节点 (node: apexly, v1.36.x)
        ├── Namespace: usearch
        │   ├── Deployment api  (image: ubuntu:26.04, glibc)
        │   │     ├── hostPath /mnt/e/data/USearch          -> /app (repo root, rw)
        │   │     ├── hostPath /mnt/e/data/USearch/.local   -> /opt/usearch (api 二进制 + lib)
        │   │     └── hostPath /var/lib/usearch/models      -> /app/.config/models (GGUF, ro)
        │   ├── Service api  (NodePort 30088 -> 8088)
        │   └── Ingress api  (api.ya.com -> service api:8088, Traefik :80)
        └── Traefik LB: 192.168.164.162  (WSL IP，重启可能漂移)
```

## 前置条件

1. **Ubuntu WSL + k3s**（systemd 已启用，`k3s kubectl get nodes` 返回 Ready）。
2. **hosts 条目**（Windows + WSL 两侧都要）：
   `192.168.164.162 api.ya.com`
3. **api 二进制**：`.local/api` 必须是 Ubuntu glibc 构建（拒绝 musl/Alpine 产物），
   `.local/lib` 内含运行时 so（libstdc++ / libgomp / libgcc_s / libssl / libcrypto）。
4. **模型**：`/var/lib/usearch/models/Nanbeige4.1-3B-Instruct.Q4_K_M.gguf`（2.44GB）。
5. **镜像**：`docker.io/library/ubuntu:26.04`（缺失时 apply.sh 会从国内镜像拉取）。

## 快速开始

```sh
# 1) 模型落盘 + 校验（幂等）
sudo deploy/k3s/fetch-model.sh

# 2) 应用部署（含配置检查 + 模型就位检查）
sudo deploy/k3s/apply.sh

# 3) 验证
curl -fsS http://api.ya.com/alive
curl -fsS http://api.ya.com/v1/models
curl -fsS -X POST http://api.ya.com/v1/chat/completions \
  -H "Authorization: Bearer sk-default" -H "Content-Type: application/json" \
  -d '{"model":"nanbeige","messages":[{"role":"user","content":"hi"}]}'
curl -fsS http://api.ya.com/mcp -H "Authorization: Bearer sk-default"
```

重建二进制后滚动重启（拷贝 `~/usearch-build/api` → 删 Pod → 等 1/1 → 探活）：

```sh
sudo deploy/k3s/_rebuild.sh          # 先重建 glibc api
sudo deploy/k3s/apply.sh redeploy    # 再滚动重启并双路径探活
```

## hosts 规则

- Windows：`C:\Windows\System32\drivers\etc\hosts` 写 `192.168.164.162 api.ya.com`，然后
  `ipconfig /flushdns`。
- WSL 侧：`/etc/hosts` 写同一行。
- **WSL 重启后 IP 可能漂移**：在 Ubuntu 内执行 `hostname -I` 拿到当前 IP，两侧同步更新；
  更新后 `k3s kubectl -n kube-system get svc traefik` 的 `EXTERNAL-IP` 应与之一致。

## 文件说明

| 文件 | 作用 |
|---|---|
| `k3s/api.yaml` | Deployment + Service(NodePort 30088) + Ingress；已含 models 挂载 + startupProbe |
| `k3s/apply.sh` | 部署入口：配置检查 + 模型检查 + apply + rollout；`redeploy` 子命令滚动重启 |
| `k3s/fetch-model.sh` | GGUF 落盘到 `/var/lib/usearch/models` + sha256 校验（幂等） |
| `k3s/_rebuild.sh` | Ubuntu glibc 增量重建 api → `.local/api` + 补齐运行时 so |
| `k3s/fix-net.sh` | 修复 Traefik/CNI 网络（iptables-legacy + 重建 kube-system Pod） |
| `k3s/depth.sh` | 部署后深度验收（鉴权/MCP//v1/stack/SSE/向量 CRUD） |

## 故障排查（本次会话 4 个根因）

1. **k3s 崩溃循环 / `bind: address already in use :6444`**
   旧进程在 PID namespace 间留下孤儿 socket。清理：Windows 侧 `wsl --shutdown` 后重进 Ubuntu，
   `systemctl restart k3s`；确认 `ss -ltnp | grep 6444` 无残留。

2. **Pod 加载 2.44GB 模型期间被杀 / 一直不 Ready**
   根因是 GGUF 放在 drvfs(`/mnt/e`) 上，9p 随机读慢 + 探针过紧。
   对策（已固化进 api.yaml）：GGUF 放 `/var/lib/usearch/models`（ext4）+ `startupProbe`
   10s×30（最长 300s），readiness/liveness 后置。先确认 `k3s kubectl -n usearch logs -l app=api`
   出现 `llama ready ...` 与 `监听 http://0.0.0.0:8088`。

3. **Traefik 503 / api.ya.com 打不通**
   几乎都是 `endpoints/api` 为空（Pod 未 Ready）。`k3s kubectl -n usearch get endpoints` 应显示
   `10.42.0.x:8088`；若空，回到根因 2。若 Traefik 本身 CrashLoop，跑 `fix-net.sh`。

4. **Alpine 与 Ubuntu 双 k3s 抢端口**
   Alpine 的 k3s 已停止并备份（`E:\do\Alpine`），不要再在 Alpine 里启 k3s。两个发行版
   共享同一 Windows 网络栈，同时跑会互相抢 6443/6444/80。

## 验证清单

- [ ] `k3s kubectl get nodes` → `Ready`
- [ ] `k3s kubectl -n usearch get pods,svc,ingress` → pod `1/1`、`endpoints/api` 有值
- [ ] `http://api.ya.com/alive` → 200
- [ ] `http://api.ya.com/v1/models` → 200
- [ ] `http://api.ya.com/v1/chat/completions`（Bearer sk-default）→ 200/流式
- [ ] `http://api.ya.com/mcp`（GET SSE + streamable-http）→ 200
- [ ] `BASE=http://api.ya.com bash deploy/k3s/depth.sh` 全绿