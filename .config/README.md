# `.config` — 唯一配置根

权威配置是 **TOML**：`config.toml`（由 `config.example.toml` 复制）。解析用 [toml++](https://github.com/marzer/tomlplusplus)。C++ 不硬编码路径或端口；缺键即启动失败。命名遵守 `rules/names.md`（单单词，禁止 `_` / `-`）。

HTTP/MCP 线协议仍为 JSON；仅进程配置与 decide 词表用 TOML。

## 布局

```text
.config/
  config.toml / config.example.toml
  decide/
    config.toml    # complex / medium 词表数组
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
| `decide.*` | 级联路由阈值；`lexicon` → `.config/decide/config.toml`（含 `complex` / `medium`） |

## 启动（WSL Ubuntu）

依赖：`build-essential`、`cmake`、`git`。可选 GPU：按 [CUDA on WSL](https://docs.nvidia.com/cuda/wsl-user-guide/index.html) 安装 toolkit（CMake 检测到则开 `GGML_CUDA`）。

```bash
cp .config/config.example.toml .config/config.toml
# 将 Nanbeige Q4_K_M 放到 .config/models/（见 config.gguf）
cmake -B build -DUSEARCH_BUILD_API=ON -DUSEARCH_BUILD_TEST_CPP=OFF -DUSEARCH_BUILD_BENCH_CPP=OFF
cmake --build build --config Release --target api -j"$(nproc)"
./build/api serve
```

大编译可把 build 放在 Linux 文件系统（如 `-B ~/usearch-build`），源码仍可在 `/mnt/e/data/USearch`。

- LLM（Codex / Claude Base URL）：`http://127.0.0.1:8088/v1`
- MCP：`http://127.0.0.1:8088/mcp`
- 路由决策：`POST /v1/route` 与 MCP 工具 `decide`（级联：model/depth/retrieval/compression/temperature）
- 决策配置：`[decide]`（参数全显式，缺键启动失败）+ `.config/decide/config.toml`

```bash
curl -s http://127.0.0.1:8088/v1/route -H 'content-type: application/json' \
  -d '{"task":"refactor auth for concurrency","files":["a.cpp"],"hints":["quality"]}'
```

`chat` / `chat/completions` 会先跑 decide：未显式传 `temperature` 时用决策温度；检索 `k` 与规则裁剪随 `retrieval` / `compression` 变化（分别来自 `decide.topk` / `keep*`）。规则激活：frontmatter `always` / `globs` + task 语义 + `manual`（对齐 apex）。
嵌入维度随 GGUF 的 `n_embd` 变化。若提示 `index dim mismatch`，删除 `index` / 重建向量库后再 upsert。

## k3s（WSL）

清单：`deploy/k3s/api.yaml`。hostPath 挂仓库 + `.local/api` + `.local/lib`（musl 依赖），镜像 `alpine:3.20`。`listen` 须为 `0.0.0.0:8088`。

同一台机器上只能有一个 k3s 占用 `:6443`（mirrored 网络下 Alpine/Ubuntu 会冲突）。用已正常的 **Ubuntu** k3s：

```bash
# Alpine 编译产物落到共享目录
mkdir -p /mnt/e/data/USearch/.local/lib
cp /root/usearch-build/api /mnt/e/data/USearch/.local/api
cp /usr/lib/libstdc++.so.6 /usr/lib/libgomp.so.1 /usr/lib/libgcc_s.so.1 \
   /usr/lib/libssl.so.3 /usr/lib/libcrypto.so.3 /mnt/e/data/USearch/.local/lib/
# Ubuntu WSL
sed 's/\r$//' deploy/k3s/apply.sh | sh
```

NodePort：`http://127.0.0.1:30088/v1` 、`/mcp`、`/alive`。

可选全量镜像见仓库根 `Dockerfile`。
