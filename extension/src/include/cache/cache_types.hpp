#pragma once

#include "duckdb.hpp"

#include <cstdint>

namespace duckdb {
namespace hbase {
namespace cache {

//! Segment 生命周期，对应方案 §7.6 的四阶段 INIT → WRITING → VALIDATING → COMMITTED。
//! INVALID 为本实现新增：完整性校验失败或文件丢失时标记，语义等同"不可用但保留排障信息"。
enum class SegmentStatus : uint8_t {
    INIT = 0,
    WRITING = 1,
    VALIDATING = 2,
    COMMITTED = 3,
    INVALID = 4,
};

const char *SegmentStatusName(SegmentStatus status);
bool TryParseSegmentStatus(const string &name, SegmentStatus &out);

//! 命中判定结果。M3 只实现 MISS / FULL_HIT，PARTIAL_HIT 为 M4 预留。
enum class CacheStatus : uint8_t {
    MISS = 0,
    PARTIAL_HIT = 1,
    FULL_HIT = 2,
};

const char *CacheStatusName(CacheStatus status);

//! 归一化后的列引用：family + qualifier（qualifier 为空表示整列族）。
//! logical_type 为 DuckDB 逻辑类型名，参与复用判定，禁止跨类型复用同一列。
struct CacheColumn {
    string family;
    string qualifier;
    string logical_type = "VARCHAR";

    //! 覆盖判定用的唯一键（用 0x1F 分隔，避免 family/qualifier 拼接歧义）。
    string Key() const;
    //! 人类可读名：family:qualifier，qualifier 为空时只返回 family。
    string DisplayName() const;
    bool operator==(const CacheColumn &other) const;
};

//! RowKey 字节序比较，与 HBase 一致（无符号字节比较，等价于 memcmp）。
//! 禁止用 std::string 的本地化/字符集语义替代。
int CompareRowKey(const string &left, const string &right);

//! 半开区间 [start, stop)。空字符串表示该侧无界：
//! start 为空 = 从表头开始，stop 为空 = 直到表尾（与 HBase Scan 的空 RowKey 语义一致）。
struct RowKeyRange {
    string start;
    string stop;

    bool IsStartUnbounded() const {
        return start.empty();
    }
    bool IsStopUnbounded() const {
        return stop.empty();
    }
    //! 两端都有界且相等时为空范围。
    bool IsEmpty() const {
        return !start.empty() && start == stop;
    }
    bool operator==(const RowKeyRange &other) const {
        return start == other.start && stop == other.stop;
    }
};

//! 版本/时间戳语义。第一版只缓存"仅最新版本、全时间范围"的扫描结果：
//! max_versions > 1 或指定 timestamp 范围的查询一律不读也不写缓存（保守，禁止误复用）。
struct ScanConsistency {
    int32_t max_versions = 1;
    int64_t timestamp_min = 0;
    int64_t timestamp_max = 0;

    bool IsCacheable() const {
        return max_versions == 1 && timestamp_min == 0 && timestamp_max == 0;
    }
    bool operator==(const ScanConsistency &other) const {
        return max_versions == other.max_versions && timestamp_min == other.timestamp_min &&
               timestamp_max == other.timestamp_max;
    }
};

//! 缓存视角的扫描计划（由 sql 层从 hbase_scan 参数构建）。
struct ScanPlan {
    string namespace_name;
    string table;
    RowKeyRange range;
    //! RequiredColumns：远端请求的全部列（M3 尚无 Filter Pushdown，等于输出列）。
    vector<CacheColumn> required_columns;
    ScanConsistency consistency;
    //! 本次查询是否允许使用缓存（`cache := false` 时为 false，同时不写缓存）。
    bool allow_cache = true;
    //! stop_row 是否闭区间；闭区间无法用半开段表示，M3 直接绕过缓存。
    bool include_stop = false;

    string TableKey() const {
        return namespace_name + "." + table;
    }
};

//! 一个缓存段的元数据，对应元数据表 cache_segment。
struct SegmentDescriptor {
    string segment_id;
    string namespace_name;
    string table;
    RowKeyRange range;
    idx_t row_count = 0;
    //! epoch 毫秒
    int64_t created_at = 0;
    //! epoch 毫秒；0 表示不过期
    int64_t expires_at = 0;
    idx_t generation = 0;
    string file_path;
    idx_t file_size = 0;
    SegmentStatus status = SegmentStatus::COMMITTED;
    ScanConsistency consistency;
    vector<CacheColumn> columns;

    string TableKey() const {
        return namespace_name + "." + table;
    }
    bool IsExpired(int64_t now_ms) const {
        return expires_at > 0 && expires_at <= now_ms;
    }
    //! 段内是否包含该列（按 Key 比较）。
    bool HasColumn(const CacheColumn &column) const;
};

//! 覆盖计划的单段描述。
struct SegmentCoverage {
    string segment_id;
    RowKeyRange range;
    vector<CacheColumn> available_columns;
};

//! Lookup 结果：必须按范围分段表达，不能用"全局列集合"判断（见 docs/cache-design.md §5）。
struct CacheLookupResult {
    CacheStatus status = CacheStatus::MISS;
    //! FULL_HIT 时按 RowKey 升序排列、互不重叠、正好铺满请求范围。
    vector<SegmentDescriptor> segments;
    //! MISS / PARTIAL_HIT 时未被任何段覆盖的子区间。
    vector<RowKeyRange> uncached_ranges;
    //! 诊断信息：为什么没有命中（不用于控制流）。
    string reason;

    //! 空范围查询（start == stop）的 FULL_HIT 允许 segments 为空：不需要读任何段，结果必然为空。
    bool IsFullHit() const {
        return status == CacheStatus::FULL_HIT;
    }
};

//! 单表缓存统计，对应 hbase_cache_stats() 的一行。
struct TableCacheStatistics {
    string namespace_name;
    string table;
    idx_t segments = 0;
    idx_t expired_segments = 0;
    idx_t rows = 0;
    idx_t bytes = 0;
    idx_t full_hit_count = 0;
    idx_t partial_hit_count = 0;
    idx_t miss_count = 0;
    idx_t remote_scan_count = 0;
    idx_t remote_get_count = 0;
    //! 缓存写入失败次数（查询结果不受影响，但必须可观测）
    idx_t write_error_count = 0;
};

//! 缓存运行时配置，来自 SET hbase_cache_*。
struct CacheConfig {
    bool enabled = true;
    int64_t ttl_seconds = 300;
    string directory = "./hbase_cache";
    //! file（默认，落盘段文件）或 memory（进程内）；其余取值显式报错。
    string mode = "file";
    //! 容量上限，M3 只解析不执行淘汰（M5）。
    string max_size = "10GB";

    bool IsValidMode() const {
        return mode == "file" || mode == "memory";
    }
};

//! 读取并校验 SET hbase_cache_* 配置。非法取值抛 InvalidInputException。
CacheConfig ReadCacheConfig(ClientContext &context);

//! 注册 hbase_cache_* 配置项（LOAD 时调用一次）。
void RegisterCacheConfiguration(DatabaseInstance &db);

//! 当前时间（epoch 毫秒）。
int64_t CurrentTimeMillis();

//! 生成全局唯一段 ID（时间戳 + 进程内自增 + 随机后缀）。
string GenerateSegmentId();

}  // namespace cache
}  // namespace hbase
}  // namespace duckdb
