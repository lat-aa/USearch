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
| L1b agent 三路径 | `API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_agent.sh` |
| L2 Apex 回合 | `API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_apex.sh` |
| L3 Hooks | `node scripts/smoke_hooks.js` |
| L4 nightly | `API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_nightly.sh` |

部署后 Ingress 探测仍用 `deploy/k3s/depth.sh`。矩阵说明见 [`.config/README.md`](.config/README.md)。

## 前置 Hook（presync）— 三端统一

服务端 `POST /v1/presync`：一次返回 `{cache, decision, rules, memory, inject, turn, block}`。
`scripts/presync.js` 按 stdin 自动适配三端**实测契约**：

| 客户端 | 事件 | 输出 | 能否注入上下文 |
|--------|------|------|----------------|
| Codex | `UserPromptSubmit` | `{hookSpecificOutput:{hookEventName,additionalContext}}` | ✅ |
| Claude | `UserPromptSubmit` | 同上 | ✅ |
| Cursor | `beforeSubmitPrompt` | `{continue:true}`（暂存上下文） | ❌（改用下面这条） |
| Cursor | `postToolUse` | `{additional_context}`（`scripts/posttool.js` 一次性消费） | ✅ |

红线：**fail-open**（服务端不可达/超时 → 直接放行，绝不阻塞输入）。

## 回合后 Hook（postsync）— 沉淀入队

`scripts/postsync.js`：**只入队**（MCP `observe`，毫秒级）→ 服务端 Worker 异步蒸馏 → SQLite + USearch 双写。

| 客户端 | 事件 | 输入 |
|--------|------|------|
| Codex | `Stop` | `transcript_path`（脚本取尾部最后一条 assistant 文本） |
| Claude | `Stop` | 同上 |
| Cursor | `afterAgentResponse` | `{text}`（assistant 最终文本，最省） |

红线：① **只入队**，蒸馏永远在 Worker；② **绝不输出** `followup_message`/`decision:block`（防自循环）；③ 幂等（同内容 90s 内只入队一次）；④ fail-open。
> 注意：Worker 蒸馏走本地模型（T600 ~5-15 tok/s），单条约 20-40s，但**异步**，不影响用户回合。'
注意：改 `hooks.json` / `.claude/settings.json` 后**需重启客户端**才生效。

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
4. **统计块：直接原样粘贴 Hook 注入的块**。presync 已把**真值**块放进上下文（`【统计块】…`：真分词器 token、`🔖` 实时栈、`💰` 无费用口径、`📦 实际注入 N tok` + 人读 corpus）；回合结束 Stop 会再补 `📝 输入 N tok · 输出 M tok` 与 `♻️ 省主 LLM api N 次，主 LLM api 实际调用 M 次` 两行（省下 = L1 缓存 + L2 缓存 + 本地直答；实调 = 真正打到上游的次数）。**禁止**用 `scripts/stats.sh` 自行重渲染（会造成"未上报 / token 为估算值"漂移、与真值不一致）。**仅当**上下文里确实没有注入块时，才用 `scripts/stats.sh` 兜底（有调 `/v1` → `-ViaV1 true` 且 `-CostModel` 对齐上游 `model`；否则 `-ViaV1 false`，🧭 = `cost.actual_model`）
5. 任务成功后调 `observe`（或 `save`）仅入队，不做重活

禁止以「太简单」跳过。失败则整链重试一次；仍失败写**工具名 + 错误原文**。禁止谎称已调用或「工具返回空」。

## 渲染

**首选：原样粘贴 Hook 注入的块**（presync 块 + Stop 补的 `📝 输入 …`/`♻️ 省主 LLM …` 两行）——它是全真值。
下面的 `scripts/stats.sh` 模板**仅作无 hook 时的兜底**；自渲染会丢失真 token / 实时栈 / 省调用计数。

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
  -TokenMode "<cost.turn.tokenMode：real|estimate；真分词器时第 6 行写「token 实测」、否则写「token 为估算值」>" \
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
🔖 决策 Nanbeige4.2 … · USearch … · SQLite … · 入队 … / 蒸馏 …
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
