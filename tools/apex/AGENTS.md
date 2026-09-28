# AGENTS.md（apex）

本文件是 **Codex 会读的仓库约定**（Cursor 的强制注入在 `.cursor/rules/apex.mdc`，改那边对 Codex 无效）。

## 客户端落点（不要混）

| 客户端 | LLM | 工具 |
|--------|-----|------|
| Cursor | Cursor 自有模型 | `http://api.ya.com/mcp`（`.cursor/mcp.json`） |
| Codex | `http://api.ya.com/v1`（`POST /v1/responses`） | **同一** `http://api.ya.com/mcp`（`.codex/config.toml` → `mcp_servers.apex`） |

鉴权：`Authorization: Bearer sk-default`（与 `.config/config.toml` 的 `token` 对齐）。

## 三端模拟自测（MCP + /v1）

本地复现 Cursor / Codex / ChatGPT 行为（不启 IDE）：

| 层 | 命令 |
|----|------|
| L0 MCP 协议 | `API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_mcp.sh` |
| L1 `/v1` 网关 | `API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_v1.sh` |
| L2 Apex 回合 | `API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_apex.sh` |
| L3 Hooks | `node scripts/test_hooks.js` |
| L4 nightly | `API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_nightly.sh` |

部署后 Ingress 探测仍用 `deploy/k3s/depth.sh`。矩阵说明见 [`.config/README.md`](.config/README.md)。

## 三平面（冻结）

| 平面 | 选模 | `model` / 🧭 |
|------|------|--------------|
| Cursor 纯 MCP | 客户端自有模型；不调 `/v1` | 无 DeepSeek 计价；🧭 = 本轮 `actual_model` |
| Codex→`/v1` | 请求体 `model`（如 `deepseek-flash` / `deepseek-v4-pro`） | **有调 `/v1` 时 🧭 = 该 API `model`** |
| MCP 工具 | `decide` 只给档位/检索建议 | 计价字段是 `model`，不是 `actual_model` |

`actual_model` 与上游 id **分列**。禁止把选择器名写入 `cost.model`。禁止把 `decide.model` 当成客户端路由名。

Codex 实模：`.codex/hooks.json` 的 `PreToolUse` 把本轮 `model` 写入 `cost`（`actual_model_source=codex-config`）。Cursor 实模由 `.cursor/hooks` 注入（`cursor-state`）。

## 政策 / 记忆 / 本地 agent

- 政策 SoT：`.config/rules`（MCP `rules` 投递）；**不是** SQLite 正文。
- 记忆 SoT：SQLite `docs` + USearch（`meta.kind=memory|cache`）；沉淀用 `observe` 入队 `queue`，Worker 后台写 memory。
- `/v1` 主路径：本地 CoT agent（`ok` 直接回 / `delegate` 转上游）；前置门控 `gate` 与 Hook(presync) 已移除。

## 硬性规则（每轮回复开始前）

含问候、一字确认、澄清——都必须：

1. `rules`（无文件则 `files=[]`、`manual=[]`；统计轮优先 `body=none`；默认吃 `decide.compression`）
2. `decide`（档位 / 检索 / 保留上下文比例）
3. `cost` — **每轮必调**；`actual_model` 优先 hook（Codex=`codex-config`，Cursor=`cursor-state`），否则请求头 / 显式 `reported`；禁止沿用上轮旧名、禁止编造、禁止本机读库脚本
4. 跑 `scripts/stats.sh`：本轮**有调 `/v1`** 时 `-ViaV1 true` 且 `-CostModel` 对齐上游 `model`；**未调 `/v1`** 时 `-ViaV1 false`，🧭 = 本轮 `cost.actual_model`；从 `cost.turn` 抄 saved/corpus/prompt
5. 任务成功后调 `observe`（或 `save`）仅入队，不做重活

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
  -PromptSource "<cost.turn.source：injected|rebuild>" \
  -PromptFile "<cost.turn.prompt（messages 数组）落入的临时文件；一行一条；渲染会自动剔除 knowledge 行>" \
  -CorpusFile "<cost.turn.corpus 落入的临时文件；人读 markdown>"
```

- 数字与栈动作一律来自工具；禁止编造
- `cost.turn` / `cost.stack` **必须抄字段**；缺 `turn.saved` 勿写「省 0 次」；缺 `corpus`/`prompt` 第 7 块写未上报
- `compression` = 上下文**保留**比例（`1.0` 全保留），禁止写成「压缩率」
- `decide.model` 只是建议档（weak/standard/strong），≠ 上游 id、≠ 客户端实模
- 禁止自绘表格 / 进度条 / ASCII 边框
- 用户需动手的事写在统计块**之前**
- 第 7 块：摘要一行 + **消息行**（仅 `cost.turn.source == injected` 时贴 `-PromptFile` —— `rebuild` 是合成内容、不得冒充真实；渲染自动剔除 `Local knowledge JSON follows` 那条）+ **人读** `turn.corpus`（`-CorpusFile`）；禁止再贴 `## rules`/`## kept`/`## pack`
- `turn.prompt`：完整 messages 数组 `[{role,content:[{type:"text",text}]}]`，knowledge JSON 在其 system 消息 `content[0].text`（禁止 `"body":"..."`）；`turn.corpus`：路由一行 + `### 规则名` 正文
- `turn.source`：`injected`（本轮 `/v1` 经 `notePrompt`）或 `rebuild`（仅 `cost` 重建）；摘要行写「注入 source」
- `turn.rules` / `turn.clip` / `turn.pack` 留给工具 JSON；**不进**统计块粘贴
- knowledge JSON 必须完整：禁止 `"body":"..."` 省略号；无真实 gate pack 时省略 `pack` 键（`gatePack 0`）
- 裁剪是否生效只看摘要行 `naive → kept` 与 `裁剪 D`；禁止 `(none)` / 「同 rules」指针段

目标七块（预设 `a`；前六行句式冻结；📦 = 摘要 + 消息行 + 人读 corpus）：

```text
⚡ 规则 **m**/**t** 命中 · token **n → o** · **省 p%**
🔖 决策 Nanbeige4.1 … · USearch … · SQLite … · 入队 … / 蒸馏 …
🧭 路由 … · … · 保留上下文 …% · 置信 …%
💰 费用 **¥…** · … · 主 LLM 省 **N** 次 · 本地 chat **M**
🏷️ 命中规则 … · 省量 筛选 **…** ＋ 裁剪 **…** · gate=… · cache=…
💡 依据 … · …
📦 上下文 保留 r% · naive N → kept K · 裁剪 D · gatePack M条/Ttok · 注入 source
## prompt
{"content":[{"text":"你是简洁助手","type":"text"}],"role":"system"}
{"content":[{"text":"…user…","type":"text"}],"role":"user"}
路由 standard · medium · L2 · 保留 70% · 置信 75%
hits 无
规则 default · names

### default
（完整正文，无 JSON 转义）

### names
（完整正文）
```

## 远端工作区

读不到本机路径时：

```sh
python E:/data/apex/scripts/sync.py E:/data/apex <workspace-id>
```

之后工具带 `workspace`；代码变更后重推。

架构：[docs/architecture.md](docs/architecture.md) · 部署：[scripts/deploy/README.md](scripts/deploy/README.md)
