#include "common/connection_registry.hpp"

#include "catch.hpp"

#include <atomic>
#include <memory>

using namespace duckdb;
using namespace duckdb::hbase;

namespace {

struct ProviderState {
    std::atomic<idx_t> close_calls {0};
};

class TrackingProvider : public IRemoteScanProvider {
public:
    explicit TrackingProvider(std::shared_ptr<ProviderState> state) : state_(std::move(state)) {
    }

    ~TrackingProvider() override {
        Close();
    }

    ProviderCapabilities GetCapabilities() override {
        return {};
    }
    RemoteSchema GetSchema(const string &, const string &) override {
        return {};
    }
    bool HealthCheck() override {
        return true;
    }
    void Scan(const RemoteScanRequest &, const RemoteBatchCallback &) override {
    }
    void BatchGet(const RemoteGetRequest &, const RemoteBatchCallback &) override {
    }
    void Cancel(const string &) override {
    }
    string Name() const override {
        return "tracking";
    }
    void Close() override {
        if (!closed_.exchange(true)) {
            state_->close_calls.fetch_add(1);
        }
    }

private:
    std::shared_ptr<ProviderState> state_;
    std::atomic<bool> closed_ {false};
};

ConnectionEntry Entry(const string &name, const std::shared_ptr<ProviderState> &state) {
    ConnectionEntry entry;
    entry.config.name = name;
    entry.provider = std::make_shared<TrackingProvider>(state);
    return entry;
}

} // namespace

TEST_CASE("覆盖连接时关闭旧 Provider 而不是新 Provider", "[connection][lifecycle]") {
    ConnectionRegistry registry;
    auto old_state = std::make_shared<ProviderState>();
    auto new_state = std::make_shared<ProviderState>();

    REQUIRE_FALSE(registry.Upsert(Entry("hbase", old_state)));
    REQUIRE(registry.Upsert(Entry("hbase", new_state)));

    REQUIRE(old_state->close_calls.load() == 1);
    REQUIRE(new_state->close_calls.load() == 0);

    ConnectionEntry current;
    REQUIRE(registry.TryGet("hbase", current));
    REQUIRE(current.provider->Name() == "tracking");
}

TEST_CASE("删除连接不关闭仍被已绑定查询持有的 Provider", "[connection][lifecycle]") {
    ConnectionRegistry registry;
    auto state = std::make_shared<ProviderState>();
    REQUIRE_FALSE(registry.Upsert(Entry("hbase", state)));

    ConnectionEntry bound_scan;
    REQUIRE(registry.TryGet("hbase", bound_scan));
    REQUIRE(registry.Remove("hbase"));
    REQUIRE(state->close_calls.load() == 0);

    bound_scan.provider.reset();
    REQUIRE(state->close_calls.load() == 1);
}
