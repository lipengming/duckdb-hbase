#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace hbase {

//! hbase_connect(connection_name, endpoint) -> BOOLEAN
void RegisterConnectFunctions(ExtensionLoader& loader);

}  // namespace hbase
}  // namespace duckdb
