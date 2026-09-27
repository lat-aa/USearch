# AGENTS.md（apex）

本文件是 **Codex 会读的仓库约定**（Cursor 的强制注入在 `.cursor/rules/apex.mdc`，改那边对 Codex 无效）。

## 客户端落点（不要混）

| 客户端 | LLM | 工具 |
|--------|-----|------|
| Cursor | Cursor 自有模型 | `http://api.ya.com/mcp`（`.cursor/mcp.json`） |
| Codex | `http://api.ya.com/v1`（`POST /v1/responses`） | **同一** `http://api.ya.com/mcp`（`.codex/config.toml` → `mcp_servers.apex`） |

鉴权：`Authorization: Bearer sk-default`（与 `.config/config.toml` 的 `token` 对齐）。

## 三平面（冻结）

| 平面 | 选模 | `model` / 🧭 |
|------|------|--------------|
| Cursor 纯 MCP | 客户端自有模型；不调 `/v1` | 无 DeepSeek 计价；🧭 = 本轮 `actual_model` |
| Codex→`/v1` | 请求体 `model`（如 `deepseek-flash` / `deepseek-v4-pro`） | **有调 `/v1` 时 🧭 = 该 API `model`** |
| MCP 工具 | `decide` 只给档位/检索建议 | 计价字段是 `model`，不是 `actual_model` |

`actual_model` 与上游 id **分列**。禁止把选择器名写入 `cost.model`。禁止把 `decide.model` 当成客户端路由名。

Codex 实模：`.codex/hooks.json` 的 `PreToolUse` 把本轮 `model` 写入 `cost`（`actual_model_source=codex-config`）。Cursor 实模由 `.cursor/hooks` 注入（`cursor-state`）。

## 政策 / 记忆 / 门控

- 政策 SoT：`.config/rules`（MCP `rules` / `gate` 投递）；**不是** SQLite 正文。
- 记忆 SoT：SQLite `docs` + USearch（`meta.kind=memory|cache`）；沉淀用 `observe` 入队 `queue`，Worker 后台写 memory。
- Hook(presync)：`scripts/presync.js` → `POST /v1/gate`；answered 可短路主 LLM；pack 注入后主 LLM 仍跑（Cursor 须首工具再调 `gate`）。

## 硬性规则（每轮回复开始前）

含问候、一字确认、澄清——都必须：

1. 若本轮未经 Hook gate：先调 `gate`（尤其 Cursor pack 路径）
2. `rules`（无文件则 `files=[]`、`manual=[]`；统计轮优先 `body=none`；默认吃 `decide.compression`）
3. `decide`（档位 / 检索 / 保留上下文比例；`decide.confidence` ≠ 门控 `answerConfidence`）
4. `cost` — **每轮必调**；`actual_model` 优先 hook（Codex=`codex-config`，Cursor=`cursor-state`），否则请求头 / 显式 `reported`；禁止沿用上轮旧名、禁止编造、禁止本机读库脚本
5. 跑 `scripts/stats.sh`：本轮**有调 `/v1`** 时 `-ViaV1 true` 且 `-CostModel` 对齐上游 `model`；**未调 `/v1`** 时 `-ViaV1 false`，🧭 = 本轮 `cost.actual_model`；从 `cost.turn` 抄 gate/saved/corpus
6. 任务成功后调 `observe`（或 `save`）仅入队，不做重活

禁止以「太简单」跳过。失败则整链重试一次；仍失败写**工具名 + 错误原文**。禁止谎称已调用或「工具返回空」。

## 渲染

``` sh
  -Total <totalRules> -Matched <matched> -Naive <naiveTokens> -Optimized <optimizedTokens> \
  -Selected <selectedTokens> -Ids "<matched ids, comma separated>" \
  -Model <model> -Depth <depth> -Retrieval <retrieval> -Confidence <confidence> \
  -Compression <compression> -Reason "<reasons[0]>" \
  -NaiveCost <naive_input_cost> -OptimizedCost <optimized_input_cost> \
  -SavedCost <saved_input_cost> -OutputCost <output_cost> -TotalCost <total_cost> \
  -CacheHit false -Peak false \
  -ViaV1 false \
  -CostModel "<有 /v1 时的上游 model，如 deepseek-flash>" \
  -ActualModel "<本轮 cost.actual_model，必填>" \
  -ActualModelSource "<reported|cursor-state|codex-config|unknown>" \
  -Nanbeige "<cost.stack.nanbeige>" \
  -USearch "<cost.stack.usearch>" \
  -Sqlite "<cost.stack.sqlite>" \
  -Gate "<cost.turn.gate>" -Cache "<cost.turn.cache>" \
  -Saved "<cost.turn.saved>" -Local "<cost.turn.local>" \
  -Queued "<cost.turn.queued 逗号拼接>" -Distill "<cost.turn.distill 逗号拼接>" \
  -Retain "<cost.turn.retain>" \
  -CtxNaive "<cost.turn.naive>" -CtxPicked "<cost.turn.picked>" -CtxKept "<cost.turn.kept>" \
  -PackTok "<cost.turn.packtok>" -PackN "<cost.turn.packn>" \
  -CorpusFile "<把 cost.turn.corpus 落入的临时文件路径>"
```

- 数字与栈动作一律来自工具；禁止编造
- `cost.turn` / `cost.stack` **必须抄字段**；缺 `turn.saved` 勿写「省 0 次」；缺 `corpus` 第 7 块写未上报
- `compression` = 上下文**保留**比例（`1.0` 全保留），禁止写成「压缩率」
- `decide.model` 只是建议档（weak/standard/strong），≠ 上游 id、≠ 客户端实模
- 禁止自绘表格 / 进度条 / ASCII 边框
- 用户需动手的事写在统计块**之前**
- 第 7 块必须贴 `cost.turn.corpus` 全文（经 `-CorpusFile`），禁止只报 token、禁止截断
- `turn.rules[]` / `turn.pack[]` 每项必有 `id` + `body`（完整正文）；`corpus` 同文，格式为 `### id` 下一行 `body` 再接全文；禁止只有 `body` 无正文、禁止 `(none)`

目标七行（预设 `a`；前六行句式冻结，只追加 turn 字段；📦 下可多行全文）：

```text
⚡ 规则 **m**/**t** 命中 · token **n → o** · **省 p%**
🔖 决策 Nanbeige4.1 … · USearch … · SQLite … · 入队 … / 蒸馏 …
🧭 路由 … · … · 保留上下文 …% · 置信 …%
💰 费用 **¥…** · … · 主 LLM 省 **N** 次 · 本地 chat **M**
🏷️ 命中规则 … · 省量 筛选 **…** ＋ 裁剪 **…** · gate=… · cache=…
💡 依据 … · …
📦 上下文 保留 r% · 全量 … · 命中 … · 注入 … · pack …tok×… 条
## rules
### default
body
（规则完整正文…）
## pack
### default
body
（同上或 gate pack 正文…）
```

## 远端工作区

读不到本机路径时：

```sh
python E:/data/apex/scripts/sync.py E:/data/apex <workspace-id>
```

之后工具带 `workspace`；代码变更后重推。

架构：[docs/architecture.md](docs/architecture.md) · 部署：[scripts/deploy/README.md](scripts/deploy/README.md)
