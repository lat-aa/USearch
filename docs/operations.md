# 运维指南（大索引）

面向生产部署：容量、mmap/load、删除与空间回收、备份与监控。
API 细节见头文件注释；本页只回答「何时做 / 怎么选」。

## 内存容量（粗算）

每个 live 节点大致：

```text
bytes ≈ key + level_byte
      + Σ_levels ( neighbor_count_header + M_level × sizeof(slot) )
      + dimensions × sizeof(scalar)   // dense 且自持向量时
```

常用量级（f32×128、`M≈16`、`connectivity_base≈32`）：

| 规模 | 粗估 RAM（索引+向量） |
|------|----------------------|
| 100 万 | ~0.7–1.2 GB |
| 1000 万 | ~7–12 GB |

实际随图连通度、删除空洞、分配器浪费浮动 ±20%。以 `memory_usage()` / `memory_stats()` 为准。

## mmap（`view`）vs 加载（`load`）

```text
只读 + 多进程共享同一文件 → view / mmap
需要增量 add/remove     → load（可写副本）
内存充足、要最低尾延迟 → load（避免冷页 page fault）
内存紧张、索引 ≫ RAM   → view，接受首查变慢
```

不可对 `view` 出来的不可变索引调用 `reclaim` / `remove`。

## 删除、isolate、compact、reclaim

| API | 做什么 | 何时用 |
|-----|--------|--------|
| `remove` | 软删：标 `free_key`，图槽仍占位 | 日常删除 |
| `isolate` | 剪掉指向已删节点的入边 | 删除后搜索开始扫死节点时 |
| `compact` | 活节点槽位重排（图局部性） | 碎片多、想改善访存 |
| **`reclaim`** | **重建**：只保留 live，释放空洞 | 删除率高、RAM/磁盘膨胀 |

删除率经验（相对 `typed_->size()` 与 `size()` 之差，或 `memory_stats` 中 wasted 占比）：

- **< 10%**：可不管  
- **10–30%**：查询变慢时 `isolate`，必要时 `compact`  
- **> 30%**，或 `vectors_wasted / vectors_allocated > 0.25`：调用 `reclaim()`（或离线导出 live 再重建）

`reclaim` 峰值约 2× 内存；大索引建议维护窗口执行。避免在热路径上对全量做 O(n²) 式逐边修补——优先 `reclaim` 重建（live 槽并行重插）。

### reclaim 运维剧本（有序）

1. **预检**：确认索引为可写 `load`（非 `view`/immutable）；记 `size()`、`memory_stats()`、删除率。
2. **备份**：`save` 全量快照到可回滚路径。
3. **排水**：停写或切只读流量；并发查询在重建期可能看到旧图，勿假设在线无抖动。
4. **执行**：调用 `reclaim()`；无 progress 回调，按规模预留墙钟时间与 2× RAM。
5. **抽检**：`size()` 对齐 live 预期；抽样 `search` / Recall 与基线对比；`memory_stats` wasted 应下降。
6. **失败**：`Can't reclaim an immutable index` → 改用可写副本；`live count mismatch` / unsupported scalar → 保留快照、查日志后离线导出 live 再 `make`。

## 备份

- `save` / 序列化为**全量快照**；无内置增量 WAL。  
- 增量写入场景：应用层定期 `save` + 外部版本/对象存储。  
- 恢复：新进程 `make(path)` 或 `view(path)`。

## 监控建议

从 `stats()` / `memory_stats()` / `size()` 采集：

- live 节点数、图边数  
- `memory_stats().{graph,vectors}_{allocated,wasted}`  
- 删除空洞：`typed_size - size`（若可观测）或业务侧删除计数  

采集频率：写入繁忙时 1–5 min；稳态 15–60 min。

## 批量查询

`search_batch(queries, n, wanted, executor)`：多查询并行，线程数受 `try_reserve` 的 `threads_search` 限制。  
单查询仍用 `search`；外层自管线程池时也可继续自行并行。
