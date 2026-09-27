# SQLite 存储层（apex 网关用）

本目录只放与 SQLite 相关的实现，和 `tools/apex/` 网关源码分开，方便上游同步与边界清晰。

| 文件 | 职责 |
|------|------|
| `sql.cpp` | `docs` 记忆正文、`queue` 入队、`audit` 旁路；与 USearch 图分离事务边界 |

由 [`tools/apex/CMakeLists.txt`](../apex/CMakeLists.txt) 编入 `api` 目标（`-DUSEARCH_BUILD_API=ON`）。声明在 `tools/apex/api.hpp`。
