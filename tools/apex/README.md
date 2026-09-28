# LLM 前置网关示例（apex）

本目录是 **USearch 核心库之外的可选 HTTP/MCP 网关**，不是 ANN 引擎本身。
启用：`-DUSEARCH_BUILD_API=ON`，产物仍为构建树根下的 `api`（`RUNTIME_OUTPUT_DIRECTORY`）。

## 职责

- `POST /v1/*`：OpenAI 兼容网关（含 `/v1/responses`）
- `POST /mcp`：MCP JSON-RPC（`gate` / `rules` / `decide` / `cost` / `observe`…）
- 本地 Nanbeige 嵌入 + SQLite 记忆（实现见 [`tools/sqlite/`](../sqlite/)）+ USearch ANN

## 构建

```sh
cmake -B build -DUSEARCH_BUILD_API=ON -DUSEARCH_BUILD_TEST_CPP=OFF -DUSEARCH_BUILD_BENCH_CPP=OFF
cmake --build build --target api
./build/api serve   # 读仓库根 .config/config.toml；k8s 须 listen=0.0.0.0:8088
```

配置说明见 [`.config/README.md`](../../.config/README.md)。AI 代理约定见本目录 [`AGENTS.md`](AGENTS.md)（根 [`AGENTS.md`](../../AGENTS.md) 指向此处）。

## 流程

政策 SoT 在 `.config/rules`（经 MCP `rules`/`gate` 投递）；SQLite 存记忆正文与 `queue`；USearch 做 ANN。工具名：`gate`、`observe`/`save`；表名：`queue`。

```text
用户输入
  ↓
【客户端】Codex / Claude / Cursor
  ↓
Hook(presync)  —— 主 LLM 之前
  Cursor : beforeSubmitPrompt → HTTP gate
  Codex  : UserPromptSubmit   → HTTP gate（或直连 /v1 时由网关内 gate）
  Claude : UserPromptSubmit   → mcp_tool gate（首选）或 HTTP gate
  ↓
MCP gate（服务端统一管线）
  ├─ L1 精确缓存（键 = hash(task)+policyFingerprint）→ 命中
  ├─ L2 语义缓存（USearch meta.kind=cache + 指纹校验）→ 命中
  │     └─→ status=answered → 见「短路出口」
  └─ 未命中 → 并行：
        ├─ 记忆：USearch(kind=memory) → SQLite 回填正文
        └─ 政策：.config/rules → resolveRules（Always/Glob 硬附加；
              Semantic 参与排序）+ SQLite 仅作冲突/禁用审计
              ↓
        （政策序 ⊕ 记忆序；Always/Glob 不进排序融合）
              ↓
        Nanbeige 门控（temperature=0；混合置信）
          answerConfidence = wevid*evidence + wself*self
          conflicts 非空 → 禁止 answered（fail-closed）
          ├─ answered（≥threshold 且无冲突且 reply 非空）→ 短路出口
          ├─ pack（置信不足 / 信息不足）→ 证据包 + 主 LLM 仍跑 → 短路出口
          └─ refuse（政策冲突）→ 展示原因，不装可答 → 短路出口

短路出口（省主 LLM,提升质量、准确度、性能、减少 token）/ pack 出口 / observe 入队 → Worker 蒸馏写 memory
```

## 自测

```sh
API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_mcp.sh
API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_v1.sh
API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_apex.sh
```

纯逻辑单测：`ctest -L apex`（见 `tests/apex/`）。
