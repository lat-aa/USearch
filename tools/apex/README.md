# LLM 前置网关示例（apex）

本目录是 **USearch 核心库之外的可选 HTTP/MCP 网关**，不是 ANN 引擎本身。
启用：`-DUSEARCH_BUILD_API=ON`，产物仍为构建树根下的 `api`（`RUNTIME_OUTPUT_DIRECTORY`）。

## 职责

- `POST /v1/*`：OpenAI 兼容网关（含 `/v1/responses`）；本地 CoT agent，必要时 `delegate` 上游
- `POST /mcp`：MCP JSON-RPC（`rules` / `decide` / `cost` / `observe`…）
- 本地 Nanbeige 嵌入 + SQLite 记忆（实现见 [`tools/sqlite/`](../sqlite/)）+ USearch ANN

## 构建

```sh
cmake -B build -DUSEARCH_BUILD_API=ON -DUSEARCH_BUILD_TEST_CPP=OFF -DUSEARCH_BUILD_BENCH_CPP=OFF
cmake --build build --target api
./build/api serve   # 读仓库根 .config/config.toml；k8s 须 listen=0.0.0.0:8088
```

配置说明见 [`.config/README.md`](../../.config/README.md)。AI 代理约定见本目录 [`AGENTS.md`](AGENTS.md)（根 [`AGENTS.md`](../../AGENTS.md) 指向此处）。

## 流程

政策 SoT 在 `.config/rules`（经 MCP `rules` 投递）；SQLite 存记忆正文与 `queue`；USearch 做 ANN。工具名：`observe`/`save`；表名：`queue`。

```text
用户输入
  ↓
【客户端】Codex / Claude / Cursor
  ↓
/v1 chat|responses（或纯 MCP 工具环：rules→decide→cost）
  ↓
本地 CoT agent（检索上下文注入 + <agent-result> 标签隔离）
  ├─ status=ok → 直接回客户端（省上游主 LLM）
  └─ status=delegate / 解析失败 → upstream.cpp 转发远端 OpenAI 兼容接口
  ↓
任务结束 → 模型调用 MCP observe（仅入队）
  ↓
SQLite 表 queue（status=pending）→ Worker 蒸馏写 memory
```

## 自测

```sh
API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_mcp.sh
API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_v1.sh
API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_apex.sh
```

纯逻辑单测：`ctest -L apex`（见 `tests/apex/`）。
