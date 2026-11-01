#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace hbase {

//! 注册 hbase_scan 表函数。
//! 当前为 M1 骨架：完成参数解析与连接校验，远端扫描在接入 RemoteScanProvider 后生效。
void RegisterScanFunctions(ExtensionLoader& loader);

}  // namespace hbase
}  // namespace duckdb
