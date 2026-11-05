// 覆盖索引单测：Full Hit 五项条件（范围、列、时间戳语义、未过期、完整性）与字节序边界。
#include "cache/cache_coverage_index.hpp"

#include "catch.hpp"

using namespace duckdb;
using namespace duckdb::hbase;
using namespace duckdb::hbase::cache;

namespace {

CacheColumn Column(const string &name, const string &type = "VARCHAR") {
    auto sep = name.find(':');
    if (sep == string::npos) {
        return CacheColumn{name, string(), type};
    }
    return CacheColumn{name.substr(0, sep), name.substr(sep + 1), type};
}

vector<CacheColumn> Columns(std::initializer_list<string> names) {
    vector<CacheColumn> columns;
    for (auto &name : names) {
        columns.push_back(Column(name));
    }
    return columns;
}

SegmentDescriptor MakeSegment(const string &id, const string &start, const string &stop,
                              vector<CacheColumn> columns, SegmentStatus status = SegmentStatus::COMMITTED,
                              int64_t expires_at = 0, int64_t row_count = 10) {
    SegmentDescriptor segment;
    segment.segment_id = id;
    segment.namespace_name = "default";
    segment.table = "t";
    segment.range.start = start;
    segment.range.stop = stop;
    segment.columns = std::move(columns);
    segment.status = status;
    segment.expires_at = expires_at;
    segment.row_count = row_count;
    segment.file_path = "/tmp/" + id + ".seg";
    return segment;
}

ScanPlan MakePlan(const string &start, const string &stop, vector<CacheColumn> columns) {
    ScanPlan plan;
    plan.namespace_name = "default";
    plan.table = "t";
    plan.range.start = start;
    plan.range.stop = stop;
    plan.required_columns = std::move(columns);
    return plan;
}

}  // namespace

TEST_CASE("单段完整覆盖请求范围 -> Full Hit", "[cache][coverage]") {
    auto columns = Columns({"cf:name", "cf:age"});
    vector<SegmentDescriptor> segments {MakeSegment("s1", "a", "z", columns)};
    auto plan = MakePlan("a", "z", columns);

    auto result = ComputeCacheLookup(segments, plan, 1000);
    REQUIRE(result.status == CacheStatus::FULL_HIT);
    REQUIRE(result.segments.size() == 1);
    REQUIRE(result.segments[0].segment_id == "s1");
    REQUIRE(result.uncached_ranges.empty());
}

TEST_CASE("相邻段拼出请求范围 -> Full Hit", "[cache][coverage]") {
    auto columns = Columns({"cf:name"});
    vector<SegmentDescriptor> segments {MakeSegment("left", "a", "m", columns),
                                        MakeSegment("right", "m", "z", columns)};
    auto plan = MakePlan("a", "z", columns);

    auto result = ComputeCacheLookup(segments, plan, 1000);
    REQUIRE(result.status == CacheStatus::FULL_HIT);
    REQUIRE(result.segments.size() == 2);
    REQUIRE(result.segments[0].segment_id == "left");
    REQUIRE(result.segments[1].segment_id == "right");
}

TEST_CASE("中间有缺口 -> Miss 并报出缺口范围", "[cache][coverage]") {
    auto columns = Columns({"cf:name"});
    vector<SegmentDescriptor> segments {MakeSegment("left", "a", "m", columns),
                                        MakeSegment("right", "p", "z", columns)};
    auto plan = MakePlan("a", "z", columns);

    auto result = ComputeCacheLookup(segments, plan, 1000);
    REQUIRE(result.status == CacheStatus::MISS);
    REQUIRE(result.segments.empty());
    REQUIRE(result.uncached_ranges.size() == 1);
    REQUIRE(result.uncached_ranges[0].start == "m");
    REQUIRE(result.uncached_ranges[0].stop == "z");
}

TEST_CASE("段范围大于请求范围时不参与覆盖（读出来会越界）", "[cache][coverage]") {
    auto columns = Columns({"cf:name"});
    vector<SegmentDescriptor> segments {MakeSegment("big", "a", "z", columns)};
    auto plan = MakePlan("b", "c", columns);

    auto result = ComputeCacheLookup(segments, plan, 1000);
    REQUIRE(result.status == CacheStatus::MISS);
}

TEST_CASE("过期段不参与覆盖", "[cache][coverage]") {
    auto columns = Columns({"cf:name"});
    vector<SegmentDescriptor> segments {MakeSegment("s1", "a", "z", columns, SegmentStatus::COMMITTED,
                                                    /*expires_at=*/900)};
    auto plan = MakePlan("a", "z", columns);

    auto result = ComputeCacheLookup(segments, plan, 1000);
    REQUIRE(result.status == CacheStatus::MISS);
    REQUIRE(result.reason.find("expired") != string::npos);
}

TEST_CASE("未过期边界：expires_at == now 视为过期", "[cache][coverage]") {
    auto columns = Columns({"cf:name"});
    vector<SegmentDescriptor> segments {
        MakeSegment("s1", "a", "z", columns, SegmentStatus::COMMITTED, /*expires_at=*/1000)};
    auto plan = MakePlan("a", "z", columns);

    REQUIRE(ComputeCacheLookup(segments, plan, 999).status == CacheStatus::FULL_HIT);
    REQUIRE(ComputeCacheLookup(segments, plan, 1000).status == CacheStatus::MISS);
}

TEST_CASE("缺列 -> Miss", "[cache][coverage]") {
    auto cached_columns = Columns({"cf:name"});
    vector<SegmentDescriptor> segments {MakeSegment("s1", "a", "z", cached_columns)};
    auto plan = MakePlan("a", "z", Columns({"cf:name", "cf:city"}));

    auto result = ComputeCacheLookup(segments, plan, 1000);
    REQUIRE(result.status == CacheStatus::MISS);
    REQUIRE(result.reason.find("columns") != string::npos);
}

TEST_CASE("列类型不同不复用", "[cache][coverage]") {
    vector<SegmentDescriptor> segments {
        MakeSegment("s1", "a", "z", {Column("cf:age", "VARCHAR")})};
    auto plan = MakePlan("a", "z", {Column("cf:age", "BIGINT")});

    REQUIRE(ComputeCacheLookup(segments, plan, 1000).status == CacheStatus::MISS);
}

TEST_CASE("非 COMMITTED 段不可见", "[cache][coverage]") {
    auto columns = Columns({"cf:name"});
    for (auto status : {SegmentStatus::INIT, SegmentStatus::WRITING, SegmentStatus::VALIDATING,
                        SegmentStatus::INVALID}) {
        vector<SegmentDescriptor> segments {MakeSegment("s1", "a", "z", columns, status)};
        auto plan = MakePlan("a", "z", columns);
        REQUIRE(ComputeCacheLookup(segments, plan, 1000).status == CacheStatus::MISS);
    }
}

TEST_CASE("版本/时间戳语义不同不复用", "[cache][coverage]") {
    auto columns = Columns({"cf:name"});
    vector<SegmentDescriptor> segments {MakeSegment("s1", "a", "z", columns)};
    auto plan = MakePlan("a", "z", columns);
    plan.consistency.max_versions = 2;

    REQUIRE(ComputeCacheLookup(segments, plan, 1000).status == CacheStatus::MISS);

    // 不可缓存的请求直接 Miss
    plan.consistency.max_versions = 1;
    plan.consistency.timestamp_min = 100;
    REQUIRE(ComputeCacheLookup(segments, plan, 1000).status == CacheStatus::MISS);
}

TEST_CASE("起点重叠但无法精确拼接时回源，避免行重复", "[cache][coverage]") {
    // [a,m) 与 [b,z) 的并集覆盖 [a,z)，但 [b,m) 会被两段同时命中 -> 行重复，必须回源
    auto columns = Columns({"cf:name"});
    vector<SegmentDescriptor> segments {MakeSegment("left", "a", "m", columns),
                                        MakeSegment("right", "b", "z", columns)};
    auto plan = MakePlan("a", "z", columns);

    auto result = ComputeCacheLookup(segments, plan, 1000);
    REQUIRE(result.status == CacheStatus::MISS);
    REQUIRE(result.segments.empty());
    REQUIRE(result.reason.find("gap") != string::npos);
}

TEST_CASE("并发写出同范围段时只取一段", "[cache][coverage]") {
    auto columns = Columns({"cf:name"});
    vector<SegmentDescriptor> segments {MakeSegment("dup1", "a", "z", columns),
                                        MakeSegment("dup2", "a", "z", columns)};
    auto plan = MakePlan("a", "z", columns);

    auto result = ComputeCacheLookup(segments, plan, 1000);
    REQUIRE(result.status == CacheStatus::FULL_HIT);
    REQUIRE(result.segments.size() == 1);
}

TEST_CASE("重叠段优先选择更长的段", "[cache][coverage]") {
    auto columns = Columns({"cf:name"});
    vector<SegmentDescriptor> segments {MakeSegment("short", "a", "c", columns),
                                        MakeSegment("long", "a", "z", columns)};
    auto plan = MakePlan("a", "z", columns);

    auto result = ComputeCacheLookup(segments, plan, 1000);
    REQUIRE(result.status == CacheStatus::FULL_HIT);
    REQUIRE(result.segments.size() == 1);
    REQUIRE(result.segments[0].segment_id == "long");
}

TEST_CASE("无界请求：单个无界段覆盖整表", "[cache][coverage]") {
    auto columns = Columns({"cf:name"});
    vector<SegmentDescriptor> segments {MakeSegment("whole", "", "", columns)};
    auto plan = MakePlan("", "", columns);

    REQUIRE(ComputeCacheLookup(segments, plan, 1000).status == CacheStatus::FULL_HIT);

    // 有界请求不能用"两头无界"的段（会读出范围外的行）
    auto bounded = MakePlan("a", "b", columns);
    REQUIRE(ComputeCacheLookup(segments, bounded, 1000).status == CacheStatus::MISS);
}

TEST_CASE("半开区间边界：stop 等于下一段 start 才算接上", "[cache][coverage]") {
    auto columns = Columns({"cf:name"});
    vector<SegmentDescriptor> segments {MakeSegment("left", "a", "m", columns),
                                        MakeSegment("right", "n", "z", columns)};
    auto plan = MakePlan("a", "z", columns);

    // [a,m) 与 [n,z) 之间缺 [m,n)
    REQUIRE(ComputeCacheLookup(segments, plan, 1000).status == CacheStatus::MISS);
}

TEST_CASE("覆盖比较使用 HBase 字节序而不是字符集排序", "[cache][coverage]") {
    auto columns = Columns({"cf:name"});
    // 0x7F < 0x80 < 0xFF（有符号 char 比较会把 0xFF 当成负数而排到前面）
    string low = string("\x7f", 1);
    string mid;
    mid.push_back(static_cast<char>(0x80));
    string high;
    high.push_back(static_cast<char>(0xFF));

    vector<SegmentDescriptor> segments {MakeSegment("s1", low, mid, columns),
                                        MakeSegment("s2", mid, high, columns)};
    auto plan = MakePlan(low, high, columns);

    auto result = ComputeCacheLookup(segments, plan, 1000);
    REQUIRE(result.status == CacheStatus::FULL_HIT);
    REQUIRE(result.segments.size() == 2);
    REQUIRE(result.segments[0].segment_id == "s1");
    REQUIRE(result.segments[1].segment_id == "s2");
}

TEST_CASE("RowKey 前缀与长度不等的比较符合 HBase 语义", "[cache][coverage]") {
    // "ab" < "abc" < "abd"
    REQUIRE(CompareRowKey("ab", "abc") < 0);
    REQUIRE(CompareRowKey("abc", "abd") < 0);
    REQUIRE(CompareRowKey("abc", "abc") == 0);
    // 含 0x00 的 RowKey："a\0b" > "a\0a"
    string left = string("a\0a", 3);
    string right = string("a\0b", 3);
    REQUIRE(CompareRowKey(left, right) < 0);
}

TEST_CASE("空范围请求直接 Full Hit 且不需要任何段", "[cache][coverage]") {
    auto columns = Columns({"cf:name"});
    vector<SegmentDescriptor> segments;
    auto plan = MakePlan("m", "m", columns);

    auto result = ComputeCacheLookup(segments, plan, 1000);
    REQUIRE(result.status == CacheStatus::FULL_HIT);
    REQUIRE(result.segments.empty());
}

TEST_CASE("没有任何列请求时不做命中判定", "[cache][coverage]") {
    ScanPlan plan;
    plan.namespace_name = "default";
    plan.table = "t";
    plan.range.start = "a";
    plan.range.stop = "z";
    REQUIRE(ComputeCacheLookup({}, plan, 1000).status == CacheStatus::MISS);
}

TEST_CASE("RangeContains 与 RangesIntersect 的边界语义", "[cache][coverage]") {
    RowKeyRange outer {"a", "z"};
    REQUIRE(RangeContains(outer, RowKeyRange {"a", "z"}));
    REQUIRE(RangeContains(outer, RowKeyRange {"b", "y"}));
    REQUIRE_FALSE(RangeContains(outer, RowKeyRange {"a", "zz"}));
    REQUIRE_FALSE(RangeContains(outer, RowKeyRange {"", "z"}));

    RowKeyRange unbounded {"", ""};
    REQUIRE(RangeContains(unbounded, outer));
    REQUIRE_FALSE(RangeContains(outer, unbounded));

    REQUIRE(RangesIntersect(RowKeyRange {"a", "m"}, RowKeyRange {"m", "z"}) == false);
    REQUIRE(RangesIntersect(RowKeyRange {"a", "n"}, RowKeyRange {"m", "z"}));
}
