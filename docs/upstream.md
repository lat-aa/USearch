# 上游同步与双轨约定

本仓库是 **USearch 核心 + apex 网关** 的产品 fork。与 [unum-cloud/USearch](https://github.com/unum-cloud/USearch) 的关系如下。

## 双轨

| 轨 | 路径 | 策略 |
|----|------|------|
| **A（可回馈上游）** | `include/usearch/`（`setup`/`util`/`sync`/`heap`/`config`/`stub`/`io`/`member`/`hnsw`，入口 `index.hpp`） | 尽量小 diff；同步上游单文件时按职责映射进上述分层。本地补丁示例：`add()` 消费 `search_to_insert_` OOM + `unlink_slot_`。 |
| **B（fork 私有）** | `include/dense/`、`include/plugins/`、`tools/apex/`、`tools/sqlite/`、`tests/apex/`、`.config/`、`.cursor/`、`.codex/`、`scripts/smoke_*`、`deploy/` | 不同步上游；上游单头 `index_dense`/`index_plugins` 在本仓库已拆到 dense/plugins。 |

根 `cpp/` 仅保留 **bench / fuzz**；网关在 `tools/apex/`；SQLite 实现在 `tools/sqlite/`。

## 一键同步 A 轨头文件

```sh
./scripts/sync_upstream.sh --check   # 只看差异
./scripts/sync_upstream.sh           # checkout upstream main 的 include/usearch
git diff -- include/usearch          # 审本地补丁是否被冲掉
```

脚本会 `git remote add upstream`（若不存在）。**禁止**假设「从未改过 header」后盲目覆盖提交。

## 暂缓项（须先测量再立项）

- 显式模板实例化 / `extern template`（与 header-only 卖点冲突）
- gate 热路径换 yyjson/simdjson（先 profile）

`dense::reclaim` 与图 `compact()` 语义不同：前者重建只保留 live；见 `docs/operations.md`。

## 构建目录约定

产物统一放在 `build/`（或 `build/<cfg>/`）；`.gitignore` 已忽略 `/build*`。k3s 部署用 `.local/api`（见 `deploy/k3s/`）。
