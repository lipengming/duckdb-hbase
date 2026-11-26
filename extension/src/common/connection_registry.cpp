#include "common/connection_registry.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {
namespace hbase {

namespace {

bool IsSupportedScheme(const string& scheme) {
    // ProviderFactory currently implements only gRPC. Reject HTTP at parse time
    // instead of accepting an endpoint that can only fail later during connect.
    return scheme == "grpc";
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
        fail("不支持的 scheme '" + config.scheme + "'，当前仅支持 grpc");
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

bool ConnectionRegistry::Upsert(ConnectionEntry entry) {
    std::shared_ptr<IRemoteScanProvider> previous_provider;
    bool replaced = false;
    {
        lock_guard<mutex> guard(lock_);
        auto name = entry.config.name;
        auto existing = connections_.find(name);
        if (existing == connections_.end()) {
            connections_.emplace(std::move(name), std::move(entry));
        } else {
            // Move the old handle out before replacing the map value. Closing the
            // iterator after insert_or_assign would close the newly installed provider.
            previous_provider = std::move(existing->second.provider);
            existing->second = std::move(entry);
            replaced = true;
        }
    }
    // Bound scans may still hold the old shared_ptr. Do not tear down their
    // channel underneath an in-flight RPC; the provider destructor performs
    // final cleanup after the last reference is released.
    if (previous_provider && previous_provider.use_count() == 1) {
        previous_provider->Close();
    }
    return replaced;
}

bool ConnectionRegistry::Remove(const string& name) {
    std::shared_ptr<IRemoteScanProvider> provider;
    {
        lock_guard<mutex> guard(lock_);
        auto entry = connections_.find(name);
        if (entry == connections_.end()) {
            return false;
        }
        provider = std::move(entry->second.provider);
        connections_.erase(entry);
    }
    // Existing bound scans retain their own provider reference. Let those scans
    // finish (or be cancelled) before the provider's destructor releases the channel.
    if (provider && provider.use_count() == 1) {
        provider->Close();
    }
    return true;
}

bool ConnectionRegistry::TryGet(const string& name, ConnectionEntry& out) const {
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
