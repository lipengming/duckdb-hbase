#!/usr/bin/env bash
# 兼容入口：统一环境检查已迁移到 scripts/env-check.sh。
# 保留本文件是为了不破坏既有文档/脚本中的引用。
exec bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/env-check.sh" "$@"
