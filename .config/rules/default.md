---
name: default
description: 构建、测试、提交与经验落盘的默认工作流
always: true
---

# 默认规则

当用户要求构建、测试或提交时：

1. 先检索本地知识与本规则，再发明新步骤。
2. 按需调用 MCP：`test`、`shell`、`status`、`commit`、`save`。
3. 任务成功后把经验归纳为 JSON，并调用 `save` 双写 Markdown 与向量库。
