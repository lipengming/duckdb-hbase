#pragma once

#include "provider/remote_scan_provider.hpp"

#include <memory>

namespace duckdb {
namespace hbase {

//! 基于 gRPC 的 RemoteScanProvider 实现。
//!
//! 头文件不暴露 gRPC 类型（PIMPL），保证 scan / cache 模块只依赖
//! remote_scan_provider.hpp，符合方案 §6.1 的依赖约束。
class GrpcProvider : public IRemoteScanProvider {
public:
    GrpcProvider(const string& host, uint16_t port, int timeout_ms);
    ~GrpcProvider() override;

    ProviderCapabilities GetCapabilities() override;
    RemoteSchema GetSchema(const string& namespace_name, const string& table) override;
    bool HealthCheck() override;
    string Name() const override;
    void Close() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hbase
}  // namespace duckdb
