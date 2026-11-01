#include "common/connection_registry.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {
namespace hbase {

namespace {

bool IsSupportedScheme(const string& scheme) {
    return scheme == "grpc" || scheme == "http";
}

bool ParsePort(const string& text, uint16_t& out) {
    if (text.empty() || text.size() > 5) {
        return false;
    }
    uint32_t value = 0;
    for (auto c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    if (value == 0 || value > 65535) {
        return false;
    }
    out = static_cast<uint16_t>(value);
    return true;
}

}  // namespace

ConnectionConfig ParseEndpoint(const string& connection_name, const string& endpoint) {
    auto fail = [&](const string& reason) {
        throw InvalidInputException("hbase_connect: 连接 '%s' 的端点 '%s' 无效：%s",
                                    connection_name, endpoint, reason);
    };

    auto scheme_end = endpoint.find("://");
    if (scheme_end == string::npos) {
        fail("缺少 scheme，期望形如 grpc://host:port");
    }

    ConnectionConfig config;
    config.name = connection_name;
    config.endpoint = endpoint;
    config.scheme = endpoint.substr(0, scheme_end);

    if (!IsSupportedScheme(config.scheme)) {
        fail("不支持的 scheme '" + config.scheme + "'，当前支持 grpc / http");
    }

    auto authority = endpoint.substr(scheme_end + 3);
    if (authority.empty()) {
        fail("缺少主机与端口");
    }

    if (authority.find('/') != string::npos) {
        fail("端点中不允许包含路径");
    }

    auto port_sep = authority.rfind(':');
    if (port_sep == string::npos) {
        fail("缺少端口");
    }

    config.host = authority.substr(0, port_sep);
    if (config.host.empty()) {
        fail("主机为空");
    }

    if (!ParsePort(authority.substr(port_sep + 1), config.port)) {
        fail("端口必须是 1-65535 的整数");
    }

    return config;
}

bool ConnectionRegistry::Upsert(ConnectionConfig config) {
    lock_guard<mutex> guard(lock_);
    auto name = config.name;
    auto inserted = connections_.insert_or_assign(std::move(name), std::move(config));
    return !inserted.second;
}

bool ConnectionRegistry::Remove(const string& name) {
    lock_guard<mutex> guard(lock_);
    return connections_.erase(name) > 0;
}

bool ConnectionRegistry::TryGet(const string& name, ConnectionConfig& out) const {
    lock_guard<mutex> guard(lock_);
    auto entry = connections_.find(name);
    if (entry == connections_.end()) {
        return false;
    }
    out = entry->second;
    return true;
}

idx_t ConnectionRegistry::Count() const {
    lock_guard<mutex> guard(lock_);
    return connections_.size();
}

ConnectionRegistry& GetConnectionRegistry(ClientContext& context) {
    auto& state_manager = *context.registered_state;
    auto state = state_manager.GetOrCreate<ConnectionRegistry>("hbase_connection_registry");
    return *state;
}

}  // namespace hbase
}  // namespace duckdb
