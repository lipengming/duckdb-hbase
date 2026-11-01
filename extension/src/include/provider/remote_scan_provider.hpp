#pragma once

#include "duckdb.hpp"

#include <cstdint>
#include <functional>

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

//! 扫描请求。RowKey 以二进制字符串承载（std::string 可容纳任意字节）。
struct RemoteScanRequest {
    string namespace_name;
    string table;
    string start_row;
    string stop_row;
    bool include_start = true;
    bool include_stop = false;
    vector<RemoteColumn> columns;
    int32_t max_versions = 1;
    int64_t timestamp_min = 0;
    int64_t timestamp_max = 0;
    int32_t batch_size = 0;
    string request_id;
};

//! 批量 Get 请求（部分缓存命中补列用）。
struct RemoteGetRequest {
    string namespace_name;
    string table;
    vector<string> rowkeys;
    vector<RemoteColumn> columns;
    int64_t timestamp_min = 0;
    int64_t timestamp_max = 0;
    string request_id;
};

//! 数据面回调：每个 ArrowBatch 触发一次。finished=true 表示流结束（payload 为空）。
using RemoteBatchCallback =
    std::function<void(const string& ipc_payload, int64_t row_count, bool finished)>;

//! 远端扫描 Provider：Extension 唯一的远端依赖边界（方案 §3.3）。
//!
//! 约定：
//! - scan/cache 模块不得 include 具体 Provider 实现头文件；
//! - 所有方法失败时抛 duckdb::IOException / duckdb::InvalidInputException；
//! - Scan/BatchGet 为阻塞式回调：实现按批回调，禁止一次性缓冲全量结果；
//! - 实现必须线程安全，可被多个查询并发调用。
class IRemoteScanProvider {
public:
    virtual ~IRemoteScanProvider() = default;

    virtual ProviderCapabilities GetCapabilities() = 0;
    virtual RemoteSchema GetSchema(const string& namespace_name, const string& table) = 0;
    virtual bool HealthCheck() = 0;

    //! 范围扫描，按批回调 Arrow IPC 载荷。
    virtual void Scan(const RemoteScanRequest& request, const RemoteBatchCallback& callback) = 0;

    //! 按 RowKey 批量读取，按批回调 Arrow IPC 载荷。
    virtual void BatchGet(const RemoteGetRequest& request, const RemoteBatchCallback& callback) = 0;

    //! Provider 类型名，例如 "grpc"。
    virtual string Name() const = 0;

    //! 释放连接资源；可重复调用。
    virtual void Close() = 0;
};

}  // namespace hbase
}  // namespace duckdb
