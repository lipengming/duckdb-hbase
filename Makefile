PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=hbase
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# ---- C++ 单元测试（缓存覆盖索引 / 段存储 / 元数据 / CacheManager）----
# 目标由 extension/CMakeLists.txt 定义，随 release 构建一起产出。
unit: release
	./build/release/extension/hbase/hbase_unit_tests
