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
| `listen` | 必须 `host:port`；k3s Pod 须 `0.0.0.0:8088`（勿绑 `127.0.0.1`） |
| `index` | USearch 图路径 |
| `base` | SQLite 载荷路径 |
| `knowledge` / `rules` / `workspace` | 目录 |
| `token` | Bearer；空=开放 |
| `shadow` | SQ8 门槛；0=总是 |
| `rate` / `refill` | 令牌桶；rate=0 关闭 |
| `chat.temperature` / `chat.max` | 采样 |
| `decide.*` | 级联路由阈值；`lexicon` → `.config/decide/config.toml`（含 `complex` / `medium`） |
| `embed.gpu` | 嵌入模型卸载层数；4G 显存下须 0(CPU)，GPU 失败会自动回退 CPU |
| `embed.gguf` | 专用嵌入模型（bge-m3 等）；空=复用 chat 模型；池化用顶层 `pooling`(cls\|mean\|lasttoken) |
| `upstream.base_url` / `upstream.key_env` / `upstream.model` | 上游兜底地址 / 密钥环境变量 / 上游 `model` 名（缺省 `https://api.deepseek.com` / `OPENAI_API_KEY` / `deepseek-chat`） |
| `agent.skip_complex` | true 时 decide 判复杂(Strong)直接走上游，不白跑本地 |
| `agent.*` | 本地 agent：`enabled`（false=直连上游）/ `max_tool_rounds` / `token_budget_ratio` / `enable_fuse` / `fuse_fail_threshold` / `fuse_recovery_seconds` |
| `cache.*` | 短路缓存：`enable_l1` / `l1_ttl_seconds` / `enable_l2` / `l2_similarity_threshold` |
| `retrieval.*` | 检索重排：`rule_weight_multiplier` / `top_k` |

## 启动（WSL Ubuntu）

依赖：`build-essential`、`cmake`、`git`。可选 GPU：按 [CUDA on WSL](https://docs.nvidia.com/cuda/wsl-user-guide/index.html) 安装 toolkit（CMake 检测到则开 `GGML_CUDA`）。网关源码在 `tools/apex/`（SQLite 在 `tools/sqlite/`）；上游同步见 [`docs/upstream.md`](../docs/upstream.md)。

```bash
cp .config/config.example.toml .config/config.toml
# 将 Nanbeige Q4_K_M 放到 .config/models/（见 config.gguf）
cmake -B build -DUSEARCH_BUILD_API=ON -DUSEARCH_BUILD_TEST_CPP=OFF -DUSEARCH_BUILD_BENCH_CPP=OFF \
  -DUSEARCH_USE_NUMKONG=ON
cmake --build build --config Release --target api -j"$(nproc)"
./build/api serve
```

NumKong 单元门禁（与 CI `quality.yml` / Ubuntu GCC 对齐）：见 `CONTRIBUTING.md`「NumKong 门禁」。

大编译可把 build 放在 Linux 文件系统（如 `-B ~/usearch-build`），源码仍可在 `/mnt/e/data/USearch`。

- LLM（Codex / Claude Base URL）：`http://127.0.0.1:8088/v1`
- MCP：`http://127.0.0.1:8088/mcp`
- 路由决策：`POST /v1/route` 与 MCP 工具 `decide`（级联：model/depth/retrieval/compression/temperature）
- 决策配置：`[decide]`（参数全显式，缺键启动失败）+ `.config/decide/config.toml`

## 三端模拟自测（MCP + /v1）

本仓 `api` 对外两套面：Streamable HTTP MCP（`/mcp`）与 OpenAI 兼容网关（`/v1/*`）。用 curl/Node 复现 Cursor / Codex / ChatGPT 语义；真客户端仅人工抽检。

| 层 | 脚本 | 覆盖 | CI |
|----|------|------|-----|
| L0 | `scripts/smoke_mcp.sh` | MCP 握手 / Auth / GET·POST SSE / `tools/list`（含 Codex 别名） | prerelease |
| L1 | `scripts/smoke_v1.sh` | `/v1` models·embed·memory·route·rules·chat·responses·流式·负向 | prerelease（无 GGUF 时生成类 SKIP） |
| Hooks | `scripts/smoke_hooks.js` | 三端 presync/posttool 契约 + fail-open（7 例） | prerelease |
| L1b | `scripts/smoke_agent.sh` | 本地 agent ok/delegate/truncated 三路径 + 工具循环 + L1 缓存 + observe→Worker | prerelease（确定性，无 GGUF/上游 key） |
| L2 | `scripts/smoke_apex.sh` | rules→decide→cost→observe→aliases→`stats.sh` | nightly |
| L3 | `scripts/test_hooks.js` | `injectmodel.js`（实际模型上报；`presync.js` gate 已移除） | prerelease |
| L4 | `scripts/smoke_nightly.sh` | L1 cache / conflict / Worker / 短路与长流式 | nightly |
| 部署 | `deploy/k3s/depth.sh` | Ingress/NodePort 探测（不替代本地 smoke） | 手工 |

一键本地（需已编 `api`）：

```bash
cmake -B build -DUSEARCH_BUILD_API=ON -DUSEARCH_BUILD_TEST_CPP=OFF -DUSEARCH_BUILD_BENCH_CPP=OFF -DUSEARCH_USE_NUMKONG=ON
cmake --build build --target api -j"$(nproc)"
API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_mcp.sh
API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_v1.sh
node scripts/smoke_hooks.js
API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_agent.sh
API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_apex.sh
node scripts/test_hooks.js
API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_nightly.sh
```

最小路径（无 token）仍可用 `scripts/smoke_api.sh`。共用起停逻辑见 `scripts/smoke_common.sh`。

```bash
curl -s http://127.0.0.1:8088/v1/route -H 'content-type: application/json' \
  -d '{"task":"refactor auth for concurrency","files":["a.cpp"],"hints":["quality"]}'
```

`chat` / `chat/completions` 会先跑 decide：未显式传 `temperature` 时用决策温度；检索 `k` 与规则裁剪随 `retrieval` / `compression` 变化（分别来自 `decide.topk` / `keep*`）。规则激活：frontmatter `always` / `globs` + task 语义 + `manual`（对齐 apex）。
嵌入维度随 GGUF 的 `n_embd` 变化。若提示 `index dim mismatch`，删除 `index` / 重建向量库后再 upsert。

## k3s（WSL Ubuntu）

清单：`deploy/k3s/api.yaml`（Deployment + NodePort + Traefik Ingress）。镜像 `ubuntu:26.04`，hostPath 挂仓库 + `.local/api` + `.local/lib`（glibc 依赖）。`listen` 须为 `0.0.0.0:8088`。`apply.sh` 会把空 `token` 写成 `sk-default`（与 Cursor MCP `Authorization: Bearer sk-default` 对齐）。

```bash
# Ubuntu：产物在 /root/usearch-build（勿在 /mnt 上 cmake）。首次编 llama；之后只编改过的 api.cpp。
# 可选加速：apt install ninja-build ccache
# 离线 llama：.local/llama-src.tar → $BUILD/_deps/llama-src（跳过 git clone）
sed 's/\r$//' deploy/k3s/_rebuild.sh | sudo -E sh
sed 's/\r$//' deploy/k3s/apply.sh | sudo -E sh
```

**hosts**：非 mirrored 网络时指向 WSL eth0 IP（`hostname -I | awk '{print $1}'`），不要用 `127.0.0.1`（Traefik LB 绑在 WSL IP 上）：

```text
192.168.164.162 api.ya.com
```

**Ingress**（`ingressClassName: traefik`，host `api.ya.com`）：

```bash
curl -sS http://api.ya.com/alive
curl -sS http://api.ya.com/v1/models -H 'Authorization: Bearer sk-default'
curl -sS -X POST http://api.ya.com/mcp \
  -H 'content-type: application/json' -H 'Authorization: Bearer sk-default' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

**客户端接入**（Bearer `sk-default`）：

- Cursor MCP：`.cursor/mcp.json` → `http://api.ya.com/mcp`
- Codex MCP：同一 `/mcp`（项目 `.codex/config.toml` 的 `mcp_servers.apex`）
- Codex LLM：只走 `http://api.ya.com/v1`（`wire_api = responses` → `POST /v1/responses`）
- 重载 MCP 后应能 `initialize` / `tools/list`（GET SSE 仅发 comment，不再推非法 `data: {}`）

若 Traefik CrashLoop / `api.ya.com:80` 不通（Ubuntu `iptables-nft` + CNI veth 悬空）：

```bash
sed 's/\r$//' deploy/k3s/fix-net.sh | sudo sh
```

备用 NodePort：`http://127.0.0.1:30088/v1` 、`/mcp`、`/alive`（鉴权同上）。

可选全量镜像见仓库根 `Dockerfile`。
