# LLM 前置网关示例（apex）

本目录是 **USearch 核心库之外的可选 HTTP/MCP 网关**，不是 ANN 引擎本身。
启用：`-DUSEARCH_BUILD_API=ON`，产物仍为构建树根下的 `api`。

## 四平面契约

| 平面 | 线程 | 允许 | 禁止 |
|------|------|------|------|
| Edge | httplib 工作线程 | auth、限流、metrics、MCP、请求级 turn | 长持 chat 锁、跑蒸馏 |
| Policy | 同请求线程 | 纯 CPU decide；rules 用缓存向量或 tryEmbed | encoder.chat；对每条 rule 阻塞 embed |
| ModelMemory | 请求或 Worker | chatMutex / embedMutex、store、SQLite、L1/L2 | Edge TU 直接 include llama |
| Async | 单 Worker | observe 只入队；闲时 distill | 用户活跃或 chatBusy!=0 时 claim/chat |

依赖单向：`Edge/Async → Policy/ModelMemory/Remote`。扁平文件，无子目录；单二进制 `api`。

## 八条硬不变量

1. observe 与 MCP cost 不得等待蒸馏结束。
2. /v1/presync 与 L2 禁止阻塞 embed；忙则 tryEmbed 空 → fail-open。
3. encoder.chat 仅 agent（用户）或 worker（且 chatBusy==0 且 lastUserMs 让路窗外）。无 chatInflight 符号，用原子 chatBusy。
4. 采样字段只在持 chatMutex 时读写。
5. presyncCalls 只计 /v1/presync；chat 另计。
6. 同线程一次只持一把 encode 锁（chat 或 embed）。
7. 规则 description 向量在加载时缓存；用户路径禁止 N+1 embed。
8. Turnstats 请求隔离：并发 Edge 不得互相覆盖 prompt/corpus。

## 词表（文件 = 平面，1 个 `.hpp`/`.cpp` 一模块）

`openai` / `mcp` / `http` / `agent` = Edge；`rules` / `decide` / `render`（含 `Fuse`）/ `types` = Policy；`encode` / `store` / `shadow` / `cache` = ModelMemory；`worker`（含 `Turnstats`）= Async；`delegate` = Remote；`config` = 装配。

头文件布局：`api.hpp` 只留 `Runtime` 装配；平面头只 `#include "types.hpp"`（`render.hpp` 为无 Runtime 的纯逻辑头）并前向声明 `Runtime`，实现 `.cpp` 才 include `api.hpp`。纯逻辑（`render`/`rules`/`shadow` 的 inline 部分）由 `ctest -L apex` 直接单测。

## 构建

```sh
cmake -B build -DUSEARCH_BUILD_API=ON -DUSEARCH_BUILD_TEST_CPP=OFF -DUSEARCH_BUILD_BENCH_CPP=OFF
cmake --build build --target api
./build/api serve
```

## 流程

政策 SoT 在 `.config/rules`（MCP `rules`）；SQLite 存记忆与 `queue`；USearch 做 ANN。

```text
/v1 或 MCP
  → Policy（decide/rules，tryEmbed 或 descVec）
  → 本地 agent（持 chatMutex）或 delegate
任务结束 → observe 仅入队
  → Worker 在 chatBusy=0 且让路窗外 distill


---- 原本早期流程禁止删除
  用户输入
  ↓
【客户端】Codex / Claude / Cursor
  ↓
Hook
  ↓
 MCP（前置同步）
  ├─ L1 内存缓存 → 命中直接返回 短路出口
  ├─ L2 语义缓存（USearch）→ 命中直接返回 短路出口
  └─ 未命中 → 并行执行：
      ├─ USearch 向量检索历史经验
      └─ SQLite 规则校验
          ↓ 混合重排（RRF）
          ↓
      Nanbeige4.2 本地推理（核心决策节点）
          ├─ 【能够处理】置信度 >= 阈值 (如 0.85) 且无规则冲突
          │   └─→ 直接生成完整回复 → 返回给用户（链路结束，节省主 LLM API 成本） 短路出口
          │
          └─ 【不能处理】置信度 < 阈值 / 信息不足 / 存在冲突
              └─→ 执行“上下文压缩” → 返回压缩后的经验片段
                  ↓
              客户端把经验片段合并进 messages 上下文
                  ↓
                  ↓
                LLM 调用（可走 api 网关，也可直连）
                  ↓
                LLM 返回 → 客户端本地工具调用循环
                  ↓
                任务结束，触发沉淀（全部只做“入队”，不做重活）
                  ↓
                Hook
                  ├─ Codex ：模型调用 MCP 工具 save（仅入队）
                  ├─ Claude：模型调用 MCP 工具 save（仅入队）
                  └─ Cursor：模型调用 MCP 工具 save（仅入队）
                  ↓
                MCP 写入 SQLite observation_queue（status=pending）
                  ↓ 立即返回  / 成功
                  ↓
                MCP 后台 Worker（常驻协程 / 线程 / 系统计划任务）
                  ├─ 原子领取任务
                  ├─ Nanbeige4.1 强蒸馏
                  ├─ 冲突检测 + 置信度合并
                  ├─ 写 SQLite 记忆表（Source of Truth）
                  └─ 写 USearch 向量索引（失败可重试 / 重建）
                  ↓
                  用户早已看到结果，沉淀在后台完成

短路出口（省主 LLM,提升质量、准确度、性能、减少 token）
```

## 观测

- `GET /v1/metrics`：按平面暴露 chatBusy、lockWait、stealChat、yieldSkip、presync
- `scripts/benchapex.sh`：presync / cost / observe / chat∥distill
- 基线：`cpp/apexbase.json`；**手动**运行 `scripts/regapex.sh` 做全量回归并比对（墙钟阈值在 CI 上抖动，故不进 CI）

## 自测

```sh
API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_mcp.sh
API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_v1.sh
API_BIN=./build/api TOKEN=sk-default ./scripts/smoke_apex.sh
# 全量回归（单元 + smoke + KPI）：
API_BIN=./build/api TOKEN=sk-default ./scripts/regapex.sh
```

纯逻辑单测：`ctest -L apex`（见 `tests/apex/`）。



