---
name: names
description: 标识命名：单单词优先，禁止下划线与中划线；符号、文件名、配置键与 MCP 工具名
globs:
  - "**/*.cpp"
  - "**/*.hpp"
  - "**/*.h"
  - "**/*.cc"
  - "**/*.cmake"
  - "**/*.toml"
  - "**/*.md"
  - "**/*.sh"
  - "**/*.yml"
  - "**/*.yaml"
  - "cmake/**"
  - ".config/**"
---

# 命名

- 禁止标识名、文件名、工具名、配置键使用 `_` 或 `-`
- 尽量一个英文单词（小写优先）：`embed`、`chat`、`serve`、`models`
- C++ 类型或成员若必须复合，用 camelCase（如 `modelReady`）；禁止蛇形或中划线标识
- 例外：第三方或协议固定路径不可改时保留（如 OpenAI 的 `/v1/chat/completions`、llama.cpp 上游 CMake 选项）
- 本仓库自有符号与 MCP 工具名必须遵守；新文件名如 `llama.cmake`（扩展名除外）
