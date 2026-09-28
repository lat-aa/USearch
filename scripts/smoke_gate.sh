#!/bin/sh
# 历史门控冒烟已退役：前置 gate /v1/gate 与 MCP gate 工具已移除。
# 请改用 smoke_v1.sh（本地 agent）与 smoke_apex.sh（rules/decide/cost/observe）。
echo "SKIP smoke_gate: gate plane removed; use smoke_v1.sh / smoke_apex.sh"
exit 0
