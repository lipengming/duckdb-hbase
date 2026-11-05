#include "cache/cache_types.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>

namespace duckdb {
namespace hbase {
namespace cache {

const char *SegmentStatusName(SegmentStatus status) {
    switch (status) {
    case SegmentStatus::INIT:
        return "INIT";
    case SegmentStatus::WRITING:
        return "WRITING";
    case SegmentStatus::VALIDATING:
        return "VALIDATING";
    case SegmentStatus::COMMITTED:
        return "COMMITTED";
    case SegmentStatus::INVALID:
        return "INVALID";
    }
    return "INVALID";
}

bool TryParseSegmentStatus(const string &name, SegmentStatus &out) {
    if (name == "INIT") {
        out = SegmentStatus::INIT;
    } else if (name == "WRITING") {
        out = SegmentStatus::WRITING;
    } else if (name == "VALIDATING") {
        out = SegmentStatus::VALIDATING;
    } else if (name == "COMMITTED") {
        out = SegmentStatus::COMMITTED;
    } else if (name == "INVALID") {
        out = SegmentStatus::INVALID;
    } else {
        return false;
    }
    return true;
}

const char *CacheStatusName(CacheStatus status) {
    switch (status) {
    case CacheStatus::MISS:
        return "MISS";
    case CacheStatus::PARTIAL_HIT:
        return "PARTIAL_HIT";
    case CacheStatus::FULL_HIT:
        return "FULL_HIT";
    }
    return "MISS";
}

string CacheColumn::Key() const {
    // 0x1F（Unit Separator）不会出现在常规列名中，避免 family="a" qualifier="b:c" 之类的拼接歧义
    string key = family;
    key.push_back('\x1f');
    key += qualifier;
    key.push_back('\x1f');
    key += logical_type;
    return key;
}

string CacheColumn::DisplayName() const {
    return qualifier.empty() ? family : family + ":" + qualifier;
}

bool CacheColumn::operator==(const CacheColumn &other) const {
    return family == other.family && qualifier == other.qualifier &&
           logical_type == other.logical_type;
}

int CompareRowKey(const string &left, const string &right) {
    auto min_size = MinValue<idx_t>(left.size(), right.size());
    if (min_size > 0) {
        int cmp = std::memcmp(left.data(), right.data(), min_size);
        if (cmp != 0) {
            return cmp < 0 ? -1 : 1;
        }
    }
    if (left.size() == right.size()) {
        return 0;
    }
    return left.size() < right.size() ? -1 : 1;
}

bool SegmentDescriptor::HasColumn(const CacheColumn &column) const {
    auto key = column.Key();
    for (auto &cached : columns) {
        if (cached.Key() == key) {
            return true;
        }
    }
    return false;
}

namespace {

bool TryGetSetting(ClientContext &context, const char *name, Value &value) {
    return static_cast<bool>(context.TryGetCurrentSetting(Identifier(name), value));
}

}  // namespace

CacheConfig ReadCacheConfig(ClientContext &context) {
    CacheConfig config;

    Value value;
    if (TryGetSetting(context, "hbase_cache_enabled", value) && !value.IsNull()) {
        config.enabled = BooleanValue::Get(value);
    }
    if (TryGetSetting(context, "hbase_cache_ttl", value) && !value.IsNull()) {
        config.ttl_seconds = BigIntValue::Get(value);
        if (config.ttl_seconds < 0) {
            throw InvalidInputException("hbase_cache_ttl 不能为负数，当前值: %lld", config.ttl_seconds);
        }
    }
    if (TryGetSetting(context, "hbase_cache_directory", value) && !value.IsNull()) {
        config.directory = StringValue::Get(value);
        if (config.directory.empty()) {
            throw InvalidInputException("hbase_cache_directory 不能为空");
        }
    }
    if (TryGetSetting(context, "hbase_cache_mode", value) && !value.IsNull()) {
        config.mode = StringUtil::Lower(StringValue::Get(value));
        if (!config.IsValidMode()) {
            throw InvalidInputException(
                "hbase_cache_mode 非法取值 '%s'，当前支持：file、memory（parquet 待实现，见 docs/roadmap.md）",
                config.mode);
        }
    }
    if (TryGetSetting(context, "hbase_cache_max_size", value) && !value.IsNull()) {
        config.max_size = StringValue::Get(value);
    }
    return config;
}

void RegisterCacheConfiguration(DatabaseInstance &db) {
    auto &config = DBConfig::GetConfig(db);
    config.AddExtensionOption("hbase_cache_enabled", "是否启用 HBase 列式 Scan Cache", LogicalType::BOOLEAN,
                              Value::BOOLEAN(true));
    config.AddExtensionOption("hbase_cache_ttl", "缓存 Segment 有效期（秒），0 表示永不过期", LogicalType::BIGINT,
                              Value::BIGINT(300));
    config.AddExtensionOption("hbase_cache_directory", "缓存根目录", LogicalType::VARCHAR,
                              Value("./hbase_cache"));
    config.AddExtensionOption("hbase_cache_mode",
                              "缓存存储模式：file（落盘段文件，默认）或 memory（进程内）。parquet 待实现",
                              LogicalType::VARCHAR, Value("file"));
    config.AddExtensionOption("hbase_cache_max_size", "缓存容量上限（M3 仅登记，淘汰在 M5 实现）",
                              LogicalType::VARCHAR, Value("10GB"));
}

int64_t CurrentTimeMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

string GenerateSegmentId() {
    static std::atomic<uint64_t> counter {0};
    static mutex random_lock;
    static std::mt19937_64 generator([]() {
        std::random_device device;
        return (static_cast<uint64_t>(device()) << 32) ^ static_cast<uint64_t>(device());
    }());

    uint64_t random_bits;
    {
        const lock_guard<mutex> guard(random_lock);
        random_bits = generator();
    }
    auto sequence = counter.fetch_add(1);
    // 时间戳保证跨进程大致有序，随机位与自增位保证同一毫秒内唯一
    return StringUtil::Format("%016llx%08llx%04llx",
                              static_cast<unsigned long long>(CurrentTimeMillis()),
                              static_cast<unsigned long long>(random_bits & 0xFFFFFFFFULL),
                              static_cast<unsigned long long>(sequence & 0xFFFFULL));
}

}  // namespace cache
}  // namespace hbase
}  // namespace duckdb
