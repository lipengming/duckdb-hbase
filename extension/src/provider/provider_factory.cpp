#include "provider/provider_factory.hpp"

#include "common/connection_registry.hpp"
#include "duckdb/common/exception.hpp"
#include "provider/grpc_provider.hpp"

namespace duckdb {
namespace hbase {

std::unique_ptr<IRemoteScanProvider> CreateProvider(const ConnectionConfig& config) {
    if (config.scheme == "grpc") {
        return make_uniq<GrpcProvider>(config.host, config.port, config.timeout_ms);
    }
    throw InvalidInputException("hbase_connect: 暂不支持 scheme '%s'（当前仅 grpc）",
                                config.scheme);
}

}  // namespace hbase
}  // namespace duckdb
