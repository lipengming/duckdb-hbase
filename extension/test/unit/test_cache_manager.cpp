// CacheManager 门面单测：BeginWrite/Finish/Lookup/OpenReader/TTL/失效/统计，
// 不依赖 Gateway（缓存闭环的核心链路）。
#include "cache/cache_manager.hpp"

#include "cache/cache_coverage_index.hpp"
#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/main/client_context.hpp"

#include <chrono>
#include <filesystem>
#include <thread>

using namespace duckdb;
using namespace duckdb::hbase;
using namespace duckdb::hbase::cache;

namespace {

class TestDirectory {
public:
    explicit TestDirectory(const string &name) {
        auto base = std::filesystem::temp_directory_path();
        path = (base / ("hbase_manager_" + name + "_" + std::to_string(counter_++))).string();
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }

    ~TestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    string path;

private:
    static inline idx_t counter_ = 0;
};

//! 建一个进程内 DuckDB 并把缓存目录/模式指到临时目录。
struct TestContext {
    explicit TestContext(const string &directory, const string &mode = "file") : db(nullptr) {
        RegisterCacheConfiguration(*db.instance);
        connection = make_uniq<Connection>(db);
        auto result = connection->Query("SET hbase_cache_directory='" + directory + "';"
                                        "SET hbase_cache_mode='" + mode + "';"
                                        "SET hbase_cache_ttl=300;");
        REQUIRE_FALSE(result->HasError());
    }

    ClientContext &Context() {
        return *connection->context;
    }

    DuckDB db;
    unique_ptr<Connection> connection;
};

CacheColumn Column(const string &name) {
    auto sep = name.find(':');
    return CacheColumn{name.substr(0, sep), name.substr(sep + 1), "VARCHAR"};
}

ScanPlan MakePlan(vector<string> names, const string &start = "a", const string &stop = "z") {
    ScanPlan plan;
    plan.namespace_name = "default";
    plan.table = "orders";
    plan.range.start = start;
    plan.range.stop = stop;
    for (auto &name : names) {
        plan.required_columns.push_back(Column(name));
    }
    return plan;
}

//! 通过写会话落一个段，返回是否成功。
bool WriteSegment(ClientContext &context, const ScanPlan &plan, const vector<vector<string>> &values,
                  const vector<vector<bool>> &valid) {
    auto &manager = CacheManager::Get(context);
    auto session = manager.BeginWrite(context, plan, plan.range);
    REQUIRE(session != nullptr);
    session->Append(values[0].size(), values, valid);
    return session->Finish();
}

}  // namespace

TEST_CASE("写段后同范围同列查询命中且读到相同数据", "[cache][manager]") {
    TestDirectory directory("hit");
    TestContext context(directory.path);
    auto &manager = CacheManager::Get(context.Context());
    auto plan = MakePlan({"cf:name", "cf:city"});

    REQUIRE(manager.Lookup(context.Context(), plan).status == CacheStatus::MISS);

    vector<vector<string>> values {{"a", "b", "c"}, {"beijing", "", "shanghai"}};
    vector<vector<bool>> valid {{true, true, true}, {true, true, false}};
    REQUIRE(WriteSegment(context.Context(), plan, values, valid));

    auto result = manager.Lookup(context.Context(), plan);
    REQUIRE(result.status == CacheStatus::FULL_HIT);
    REQUIRE(result.segments.size() == 1);
    REQUIRE(result.segments[0].row_count == 3);

    auto reader = manager.OpenReader(result.segments[0]);
    REQUIRE(reader->RowCount() == 3);
    CacheBatch batch;
    REQUIRE(reader->Next(batch));
    REQUIRE(batch.row_count == 3);
    REQUIRE(batch.columns[0][0] == "a");
    REQUIRE(batch.columns[1][0] == "beijing");
    REQUIRE(batch.valid[1][2] == false);
}

TEST_CASE("列集合变化不命中（M3 未实现 Partial Hit）", "[cache][manager]") {
    TestDirectory directory("partial");
    TestContext context(directory.path);
    auto &manager = CacheManager::Get(context.Context());

    auto base = MakePlan({"cf:name"});
    vector<vector<string>> values {{"a"}};
    vector<vector<bool>> valid {{true}};
    REQUIRE(WriteSegment(context.Context(), base, values, valid));

    REQUIRE(manager.Lookup(context.Context(), base).status == CacheStatus::FULL_HIT);
    auto extended = MakePlan({"cf:name", "cf:age"});
    REQUIRE(manager.Lookup(context.Context(), extended).status == CacheStatus::MISS);
}

TEST_CASE("请求范围小于已缓存段时不命中（无 RowKey 无法裁剪）", "[cache][manager]") {
    TestDirectory directory("subset");
    TestContext context(directory.path);
    auto &manager = CacheManager::Get(context.Context());

    auto wide = MakePlan({"cf:name"}, "a", "z");
    vector<vector<string>> values {{"a", "b"}};
    vector<vector<bool>> valid {{true, true}};
    REQUIRE(WriteSegment(context.Context(), wide, values, valid));

    auto narrow = MakePlan({"cf:name"}, "b", "c");
    REQUIRE(manager.Lookup(context.Context(), narrow).status == CacheStatus::MISS);
}

TEST_CASE("cache := false 既不读也不写缓存", "[cache][manager]") {
    TestDirectory directory("disabled");
    TestContext context(directory.path);
    auto &manager = CacheManager::Get(context.Context());

    auto plan = MakePlan({"cf:name"});
    plan.allow_cache = false;
    REQUIRE(manager.BeginWrite(context.Context(), plan, plan.range) == nullptr);
    REQUIRE(manager.Lookup(context.Context(), plan).status == CacheStatus::MISS);

    // 全局开关关闭同样不写
    auto disabled = context.connection->Query("SET hbase_cache_enabled=false;");
    REQUIRE_FALSE(disabled->HasError());
    REQUIRE(manager.BeginWrite(context.Context(), plan, plan.range) == nullptr);
}

TEST_CASE("多版本/时间戳语义不缓存", "[cache][manager]") {
    TestDirectory directory("consistency");
    TestContext context(directory.path);
    auto &manager = CacheManager::Get(context.Context());

    auto plan = MakePlan({"cf:name"});
    plan.consistency.max_versions = 3;
    REQUIRE(manager.BeginWrite(context.Context(), plan, plan.range) == nullptr);
    REQUIRE(manager.Lookup(context.Context(), plan).status == CacheStatus::MISS);
}

TEST_CASE("TTL 到期后立即降级为 Miss", "[cache][manager]") {
    TestDirectory directory("ttl");
    TestContext context(directory.path);
    auto &manager = CacheManager::Get(context.Context());
    auto set_ttl = context.connection->Query("SET hbase_cache_ttl=1;");
    REQUIRE_FALSE(set_ttl->HasError());

    auto plan = MakePlan({"cf:name"});
    vector<vector<string>> values {{"a"}};
    vector<vector<bool>> valid {{true}};
    REQUIRE(WriteSegment(context.Context(), plan, values, valid));
    REQUIRE(manager.Lookup(context.Context(), plan).status == CacheStatus::FULL_HIT);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    REQUIRE(manager.Lookup(context.Context(), plan).status == CacheStatus::MISS);
    // 过期段仍在统计中可见
    auto statistics = manager.GetStatistics(context.Context());
    REQUIRE(statistics.size() == 1);
    REQUIRE(statistics[0].expired_segments == 1);
    REQUIRE(statistics[0].segments == 0);
}

TEST_CASE("hbase_cache_clear 语义：按表与全库失效", "[cache][manager]") {
    TestDirectory directory("clear");
    TestContext context(directory.path);
    auto &manager = CacheManager::Get(context.Context());

    auto plan = MakePlan({"cf:name"});
    vector<vector<string>> values {{"a"}};
    vector<vector<bool>> valid {{true}};
    REQUIRE(WriteSegment(context.Context(), plan, values, valid));
    REQUIRE(manager.Lookup(context.Context(), plan).status == CacheStatus::FULL_HIT);

    REQUIRE(manager.InvalidateTable(context.Context(), "default", "orders") == 1);
    REQUIRE(manager.Lookup(context.Context(), plan).status == CacheStatus::MISS);

    REQUIRE(WriteSegment(context.Context(), plan, values, valid));
    REQUIRE(manager.InvalidateAll(context.Context()) == 1);
    REQUIRE(manager.Lookup(context.Context(), plan).status == CacheStatus::MISS);
}

TEST_CASE("统计口径：命中/未命中/远端扫描计数", "[cache][manager]") {
    TestDirectory directory("stats");
    TestContext context(directory.path);
    auto &manager = CacheManager::Get(context.Context());
    auto plan = MakePlan({"cf:name"});

    manager.Lookup(context.Context(), plan);  // miss
    manager.RecordRemoteScan("default", "orders");
    vector<vector<string>> values {{"a"}};
    vector<vector<bool>> valid {{true}};
    REQUIRE(WriteSegment(context.Context(), plan, values, valid));
    manager.Lookup(context.Context(), plan);  // full hit

    auto statistics = manager.GetStatistics(context.Context());
    REQUIRE(statistics.size() == 1);
    auto &stats = statistics[0];
    REQUIRE(stats.namespace_name == "default");
    REQUIRE(stats.table == "orders");
    REQUIRE(stats.miss_count == 1);
    REQUIRE(stats.full_hit_count == 1);
    REQUIRE(stats.remote_scan_count == 1);
    REQUIRE(stats.segments == 1);
    REQUIRE(stats.rows == 1);
    REQUIRE(stats.bytes > 0);
}

TEST_CASE("memory 模式同样闭环，且不与 file 模式串味", "[cache][manager]") {
    TestDirectory directory("memory");
    TestContext context(directory.path, "memory");
    auto &manager = CacheManager::Get(context.Context());
    auto plan = MakePlan({"cf:name"});

    vector<vector<string>> values {{"a", "b"}};
    vector<vector<bool>> valid {{true, true}};
    REQUIRE(WriteSegment(context.Context(), plan, values, valid));

    auto result = manager.Lookup(context.Context(), plan);
    REQUIRE(result.status == CacheStatus::FULL_HIT);
    REQUIRE(result.segments.size() == 1);
    REQUIRE(StringUtil::StartsWith(result.segments[0].file_path, "memory://"));

    auto reader = manager.OpenReader(result.segments[0]);
    CacheBatch batch;
    REQUIRE(reader->Next(batch));
    REQUIRE(batch.row_count == 2);

    // 切回 file 模式：memory 段不应参与覆盖
    auto switched = context.connection->Query("SET hbase_cache_mode='file';");
    REQUIRE_FALSE(switched->HasError());
    REQUIRE(manager.Lookup(context.Context(), plan).status == CacheStatus::MISS);
}

TEST_CASE("非法 hbase_cache_mode 显式报错", "[cache][manager]") {
    TestDirectory directory("bad_mode");
    TestContext context(directory.path);
    auto result = context.connection->Query("SET hbase_cache_mode='parquet';"
                                            "SELECT * FROM hbase_cache_stats();");
    // 未加载扩展时函数不存在，这里只校验 SET 之后的配置读取会拒绝非法值
    CacheManager manager(context.Context());
    REQUIRE_THROWS_AS(manager.GetConfig(context.Context()), InvalidInputException);
}
