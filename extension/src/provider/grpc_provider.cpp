#include "provider/grpc_provider.hpp"

#include "duckdb/common/exception.hpp"

#include "hbase_data_service.grpc.pb.h"
#include "hbase_data_service.pb.h"

#include <chrono>
#include <grpcpp/grpcpp.h>
#include <memory>

namespace duckdb {
namespace hbase {

struct GrpcProvider::Impl {
    string target;
    int timeout_ms = 30000;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<v1::HBaseDataService::Stub> stub;

    void EnsureStub() {
        if (!channel) {
            channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
        }
        if (!stub) {
            stub = v1::HBaseDataService::NewStub(channel);
        }
    }

    //! 每个 RPC 使用独立 deadline，避免一个慢请求拖垮整条连接。
    std::unique_ptr<grpc::ClientContext> NewContext() {
        auto context = std::make_unique<grpc::ClientContext>();
        auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms);
        context->set_deadline(deadline);
        return context;
    }
};

GrpcProvider::GrpcProvider(const string& host, uint16_t port, int timeout_ms) : impl_(new Impl()) {
    impl_->target = host + ":" + std::to_string(port);
    impl_->timeout_ms = timeout_ms > 0 ? timeout_ms : 30000;
    impl_->EnsureStub();
}

GrpcProvider::~GrpcProvider() {
    Close();
}

ProviderCapabilities GrpcProvider::GetCapabilities() {
    impl_->EnsureStub();
    v1::CapabilityRequest request;
    v1::CapabilityResponse response;
    auto context = impl_->NewContext();
    auto status = impl_->stub->GetCapabilities(context.get(), request, &response);
    if (!status.ok()) {
        throw IOException("Gateway GetCapabilities 失败 (%s): %s", impl_->target,
                          status.error_message());
    }

    ProviderCapabilities capabilities;
    capabilities.supports_scan = response.supports_scan();
    capabilities.supports_batch_get = response.supports_batch_get();
    capabilities.supports_rowkey_range = response.supports_rowkey_range();
    capabilities.supports_column_projection = response.supports_column_projection();
    capabilities.supports_timestamp = response.supports_timestamp();
    capabilities.supports_versions = response.supports_versions();
    capabilities.supports_filter_pushdown = response.supports_filter_pushdown();
    capabilities.supports_async_scan = response.supports_async_scan();
    capabilities.supports_cancellation = response.supports_cancellation();
    capabilities.max_batch_size = response.max_batch_size();
    capabilities.max_parallel_scans = response.max_parallel_scans();
    return capabilities;
}

RemoteSchema GrpcProvider::GetSchema(const string& namespace_name, const string& table) {
    impl_->EnsureStub();
    v1::SchemaRequest request;
    request.set_namespace_(namespace_name);
    request.set_table(table);

    v1::SchemaResponse response;
    auto context = impl_->NewContext();
    auto status = impl_->stub->GetSchema(context.get(), request, &response);
    if (!status.ok()) {
        throw IOException("Gateway GetSchema 失败 (%s, %s.%s): %s", impl_->target, namespace_name,
                          table, status.error_message());
    }

    RemoteSchema schema;
    schema.namespace_name = namespace_name;
    schema.table = table;
    for (auto& column : response.columns()) {
        schema.columns.push_back(
            RemoteColumn{column.family(), column.qualifier(), column.sql_type()});
    }
    return schema;
}

bool GrpcProvider::HealthCheck() {
    try {
        impl_->EnsureStub();
        v1::HealthRequest request;
        v1::HealthResponse response;
        auto context = impl_->NewContext();
        auto status = impl_->stub->Health(context.get(), request, &response);
        return status.ok() && response.status() == "serving";
    } catch (...) {
        return false;
    }
}

string GrpcProvider::Name() const {
    return "grpc";
}

void GrpcProvider::Close() {
    if (!impl_) {
        return;
    }
    impl_->stub.reset();
    impl_->channel.reset();
}

}  // namespace hbase
}  // namespace duckdb
