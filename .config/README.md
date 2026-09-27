# `.config` — 唯一配置根

权威配置是本目录下的 `config.json`（由 `config.example.json` 复制）。C++ 不硬编码路径或端口；缺键即启动失败。命名遵守 `rules/names.md`（单单词，禁止 `_` / `-`）。

## 布局

```text
.config/
  config.json / config.example.json
  models/          # 仅 GGUF 等大文件
  knowledge/       # 经验 Markdown
  rules/           # 规则 *.md（含 names.md）
  index.usearch    # 由 config.index 指定
  store.sqlite     # 由 config.base 指定
```

## 键（单单词）

| 键 | 含义 |
|----|------|
| `gguf` | 权重路径（相对仓库根） |
| `ctx` / `gpu` / `threads` | 推理上下文；`gpu=-1` 全层卸载；`threads=0` 用硬件并发 |
| `pooling` | 须为 `lasttoken` |
| `listen` | 必须 `host:port` |
| `index` | USearch 图路径 |
| `base` | SQLite 载荷路径 |
| `knowledge` / `rules` / `workspace` | 目录 |
| `token` | Bearer；空=开放 |
| `shadow` | SQ8 门槛；0=总是 |
| `rate` / `refill` | 令牌桶；rate=0 关闭 |
| `chat.temperature` / `chat.max` | 采样 |

## 启动（WSL Ubuntu）

依赖：`build-essential`、`cmake`、`git`。可选 GPU：按 [CUDA on WSL](https://docs.nvidia.com/cuda/wsl-user-guide/index.html) 安装 toolkit（CMake 检测到则开 `GGML_CUDA`）。

```bash
cp .config/config.example.json .config/config.json
# 将 Nanbeige Q4_K_M 放到 .config/models/（见 config.gguf）
cmake -B build -DUSEARCH_BUILD_API=ON -DUSEARCH_BUILD_TEST_CPP=OFF -DUSEARCH_BUILD_BENCH_CPP=OFF
cmake --build build --config Release --target api -j"$(nproc)"
./build/api serve
```

大编译可把 build 放在 Linux 文件系统（如 `-B ~/usearch-build`），源码仍可在 `/mnt/e/data/USearch`。

- LLM（Codex / Claude Base URL）：`http://127.0.0.1:8088/v1`
- MCP：`http://127.0.0.1:8088/mcp`

嵌入维度随 GGUF 的 `n_embd` 变化。若提示 `index dim mismatch`，删除 `index` / 重建向量库后再 upsert。

## k3s（WSL）

清单：`deploy/k3s/api.yaml`（hostPath 挂仓库 + `.local/api`，镜像 `alpine:3.20`）。`listen` 须为 `0.0.0.0:8088`。

同一台机器上只能有一个 k3s 占用 `:6443`（mirrored 网络下 Alpine/Ubuntu 会抢端口）。优先用已正常的 Ubuntu k3s：

```bash
# Alpine 上编好后拷到共享目录
cp /root/usearch-build/api /mnt/e/data/USearch/.local/api
# Ubuntu WSL
tr -d '\r' < deploy/k3s/apply.sh | sh
```

NodePort：`http://127.0.0.1:30088/v1` 、`/mcp`、`/alive`。

可选全量镜像构建见仓库根 `Dockerfile`（需 docker + `USEARCH_K3S_IMAGE=1`）。
