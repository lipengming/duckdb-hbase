#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace hbase {

//! 注册 hbase_cache_stats() 与 hbase_cache_clear()，以及 hbase_cache_* 配置项。
void RegisterCacheFunctions(ExtensionLoader &loader);

}  // namespace hbase
}  // namespace duckdb
