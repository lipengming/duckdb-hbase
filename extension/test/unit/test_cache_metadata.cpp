// 元数据存储单测：持久化、二进制 RowKey、列清单、原子删除与崩溃残留清理。
#include "cache/cache_metadata_store.hpp"

#include "catch.hpp"
#include "duckdb/common/file_system.hpp"

#include <filesystem>

using namespace duckdb;
using namespace duckdb::hbase;
using namespace duckdb::hbase::cache;

namespace {

class TestDirectory {
public:
    explicit TestDirectory(const string &name) {
        auto base = std::filesystem::temp_directory_path();
        path = (base / ("hbase_meta_" + name + "_" + std::to_string(counter_++))).string();
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

SegmentDescriptor MakeSegment(const string &id, const string &namespace_name, const string &table,
                              const string &start, const string &stop, SegmentStatus status,
                              int64_t expires_at = 0) {
    SegmentDescriptor segment;
    segment.segment_id = id;
    segment.namespace_name = namespace_name;
    segment.table = table;
    segment.range.start = start;
    segment.range.stop = stop;
    segment.row_count = 42;
    segment.created_at = 1000;
    segment.expires_at = expires_at;
    segment.generation = 3;
    segment.file_path = "/cache/" + id + ".seg";
    segment.file_size = 4096;
    segment.status = status;
    segment.consistency.max_versions = 1;
    segment.columns = {CacheColumn{"cf", "name", "VARCHAR"}, CacheColumn{"cf", "age", "BIGINT"}};
    return segment;
}

}  // namespace

TEST_CASE("元数据提交后可读回，且包含二进制 RowKey", "[cache][metadata]") {
    TestDirectory directory("roundtrip");
    auto fs = FileSystem::CreateLocal();
    {
        CacheMetadataStore store(*fs, directory.path);
        string start;
        start.push_back('\0');
        start.push_back(static_cast<char>(0xFF));
        string stop = "zzz";
        store.CommitSegment(MakeSegment("s1", "default", "t1", start, stop, SegmentStatus::COMMITTED,
                                        /*expires_at=*/999999));

        vector<SegmentDescriptor> segments;
        store.LoadSegments("default", "t1", segments);
        REQUIRE(segments.size() == 1);
        auto &segment = segments[0];
        REQUIRE(segment.segment_id == "s1");
        REQUIRE(segment.range.start == start);
        REQUIRE(segment.range.stop == stop);
        REQUIRE(segment.row_count == 42);
        REQUIRE(segment.generation == 3);
        REQUIRE(segment.file_size == 4096);
        REQUIRE(segment.status == SegmentStatus::COMMITTED);
        REQUIRE(segment.expires_at == 999999);
        REQUIRE(segment.columns.size() == 2);
        REQUIRE(segment.HasColumn(CacheColumn{"cf", "age", "BIGINT"}));
    }

    // 新实例（模拟进程重启）仍能读回
    {
        CacheMetadataStore store(*fs, directory.path);
        vector<SegmentDescriptor> segments;
        store.LoadSegments("default", "t1", segments);
        REQUIRE(segments.size() == 1);
        REQUIRE(segments[0].columns.size() == 2);
    }
}

TEST_CASE("按表隔离：不同 namespace/表互不影响", "[cache][metadata]") {
    TestDirectory directory("tables");
    auto fs = FileSystem::CreateLocal();
    CacheMetadataStore store(*fs, directory.path);
    store.CommitSegment(MakeSegment("s1", "default", "t1", "a", "m", SegmentStatus::COMMITTED));
    store.CommitSegment(MakeSegment("s2", "default", "t1", "m", "z", SegmentStatus::COMMITTED));
    store.CommitSegment(MakeSegment("s3", "prod", "t1", "a", "z", SegmentStatus::COMMITTED));

    vector<SegmentDescriptor> segments;
    store.LoadSegments("default", "t1", segments);
    REQUIRE(segments.size() == 2);

    store.LoadSegments("prod", "t1", segments);
    REQUIRE(segments.size() == 1);
    REQUIRE(segments[0].segment_id == "s3");

    // 删除单表只影响该表
    auto removed = store.DeleteSegmentsOfTable("default", "t1");
    REQUIRE(removed.size() == 2);
    store.LoadSegments("default", "t1", segments);
    REQUIRE(segments.empty());
    store.LoadSegments("prod", "t1", segments);
    REQUIRE(segments.size() == 1);
}

TEST_CASE("清理未提交记录：崩溃残留的 WRITING 段不可见且被回收", "[cache][metadata]") {
    TestDirectory directory("uncommitted");
    auto fs = FileSystem::CreateLocal();
    CacheMetadataStore store(*fs, directory.path);
    store.CommitSegment(MakeSegment("committed", "default", "t1", "a", "z", SegmentStatus::COMMITTED));
    store.CommitSegment(MakeSegment("crashed", "default", "t1", "a", "z", SegmentStatus::WRITING));
    store.CommitSegment(MakeSegment("broken", "default", "t1", "a", "z", SegmentStatus::INVALID));

    auto cleaned = store.CleanupUncommitted();
    REQUIRE(cleaned.size() == 2);

    vector<SegmentDescriptor> segments;
    store.LoadSegments("default", "t1", segments);
    REQUIRE(segments.size() == 1);
    REQUIRE(segments[0].segment_id == "committed");
}

TEST_CASE("全库清理返回全部段描述符", "[cache][metadata]") {
    TestDirectory directory("clear_all");
    auto fs = FileSystem::CreateLocal();
    CacheMetadataStore store(*fs, directory.path);
    store.CommitSegment(MakeSegment("s1", "default", "t1", "a", "m", SegmentStatus::COMMITTED));
    store.CommitSegment(MakeSegment("s2", "prod", "t2", "m", "z", SegmentStatus::COMMITTED));

    auto removed = store.DeleteAllSegments();
    REQUIRE(removed.size() == 2);

    vector<SegmentDescriptor> segments;
    store.LoadAllSegments(segments);
    REQUIRE(segments.empty());
}

TEST_CASE("MarkStatus 让段立即不可复用", "[cache][metadata]") {
    TestDirectory directory("mark");
    auto fs = FileSystem::CreateLocal();
    CacheMetadataStore store(*fs, directory.path);
    store.CommitSegment(MakeSegment("s1", "default", "t1", "a", "z", SegmentStatus::COMMITTED));
    store.MarkStatus("s1", SegmentStatus::INVALID);

    vector<SegmentDescriptor> segments;
    store.LoadSegments("default", "t1", segments);
    REQUIRE(segments.size() == 1);
    REQUIRE(segments[0].status == SegmentStatus::INVALID);
}
