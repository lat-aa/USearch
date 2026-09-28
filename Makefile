# 本地质量门禁。
#   make check       静态检查 + apex 单测（快速，无需 api 二进制）
#   make check-all   check + 端到端 smoke（需先构建 api）
#   make check-e2e   仅端到端 smoke（hash 模式，确定性）
#
# e2e 前置构建：cmake -B build -D USEARCH_BUILD_API=ON && cmake --build build --target api

.PHONY: check lint test check-e2e check-all

check: lint test

lint:
	./scripts/check.sh lint

test:
	./scripts/check.sh unit

check-e2e:
	./scripts/check.sh e2e

check-all: check check-e2e
