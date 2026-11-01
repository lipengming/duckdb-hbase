#pragma once

#include "provider/remote_scan_provider.hpp"

#include <memory>

namespace duckdb {
namespace hbase {

struct ConnectionConfig;

//! 按连接配置创建 Provider。
//! 当前只支持 grpc；HTTP Provider 在方案 §3.8 中列为可替换实现，待 M2 加入。
//! 调用方负责在失败时处理异常（端点不可达、握手失败等）。
std::unique_ptr<IRemoteScanProvider> CreateProvider(const ConnectionConfig& config);

}  // namespace hbase
}  // namespace duckdb
