#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace hbase {

//! Provider 能力声明，对应协议 CapabilityResponse 与方案 §3.4。
struct ProviderCapabilities {
    bool supports_scan = false;
    bool supports_batch_get = false;
    bool supports_rowkey_range = false;
    bool supports_column_projection = false;
    bool supports_timestamp = false;
    bool supports_versions = false;
    bool supports_filter_pushdown = false;
    bool supports_async_scan = false;
    bool supports_cancellation = false;
    int32_t max_batch_size = 0;
    int32_t max_parallel_scans = 0;
};

//! 远端列描述。qualifier 为空表示整列族；sql_type 为 DuckDB 逻辑类型名。
struct RemoteColumn {
    string family;
    string qualifier;
    string sql_type;
};

struct RemoteSchema {
    string namespace_name;
    string table;
    vector<RemoteColumn> columns;
};

//! 远端扫描 Provider：Extension 唯一的远端依赖边界（方案 §3.3）。
//!
//! 约定：
//! - scan 不得 include 本文件之外的任何具体 Provider 实现头文件；
//! - 所有方法在失败时抛 duckdb::IOException / duckdb::InvalidInputException；
//! - 实现必须线程安全，可被多个 Scan 线程并发调用。
class IRemoteScanProvider {
public:
    virtual ~IRemoteScanProvider() = default;

    //! 连接建立后协商能力；失败抛异常。
    virtual ProviderCapabilities GetCapabilities() = 0;

    //! 获取表结构（列族 + 已知 qualifier + 逻辑类型）。
    virtual RemoteSchema GetSchema(const string& namespace_name, const string& table) = 0;

    //! 健康检查；不应抛异常，不可用时返回 false。
    virtual bool HealthCheck() = 0;

    //! Provider 类型名，例如 "grpc"。
    virtual string Name() const = 0;

    //! 释放连接资源；可重复调用。
    virtual void Close() = 0;
};

}  // namespace hbase
}  // namespace duckdb
