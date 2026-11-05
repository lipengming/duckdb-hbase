#include "cache/cache_manager.hpp"

#include "cache/cache_coverage_index.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"

#include <exception>

namespace duckdb {
namespace hbase {
namespace cache {

namespace {

//! 统计键：namespace 与表名用 0x1F 分隔，避免 "a.b" + "c" 与 "a" + "b.c" 撞键。
string StatisticsKey(const string &namespace_name, const string &table) {
    return namespace_name + "\x1f" + table;
}

bool IsMemoryPath(const string &path) {
    return StringUtil::StartsWith(path, "memory://");
}

}  // namespace

// ---------------------------------------------------------------------------
// CacheWriteSession
// ---------------------------------------------------------------------------

CacheWriteSession::CacheWriteSession(CacheManager &manager, ScanPlan plan, SegmentDescriptor segment,
                                     unique_ptr<ICacheSegmentWriter> writer)
    : manager_(manager), plan_(std::move(plan)), segment_(std::move(segment)), writer_(std::move(writer)) {
}

CacheWriteSession::~CacheWriteSession() {
    if (!finished_) {
        Abort();
    }
}

void CacheWriteSession::Append(idx_t row_count, const vector<vector<string>> &values,
                               const vector<vector<bool>> &valid) {
    if (finished_ || !writer_) {
        return;
    }
    try {
        writer_->Append(row_count, values, valid);
        segment_.row_count += row_count;
    } catch (std::exception &) {
        // 写缓存失败不能中断查询：记录统计、放弃本段，查询继续从远端返回数据
        manager_.RecordWriteError(plan_.namespace_name, plan_.table);
        Abort();
    }
}

bool CacheWriteSession::Finish() {
    if (finished_) {
        return false;
    }
    finished_ = true;
    if (!writer_) {
        return false;
    }
    try {
        segment_.file_path = writer_->Finish();
        segment_.file_size = writer_->FileSize();
        segment_.status = SegmentStatus::COMMITTED;
        manager_.CommitSegment(segment_);
        writer_.reset();
        return true;
    } catch (std::exception &) {
        // 缓存是加速手段，写失败绝不能影响查询结果：记录统计并清理残留
        manager_.RecordWriteError(plan_.namespace_name, plan_.table);
        try {
            writer_->Abort();
        } catch (...) {  // NOLINT: 清理失败不覆盖原始错误
        }
        writer_.reset();
        return false;
    }
}

void CacheWriteSession::Abort() {
    if (finished_) {
        return;
    }
    finished_ = true;
    if (writer_) {
        try {
            writer_->Abort();
        } catch (...) {  // NOLINT: 取消路径不允许抛出
        }
        writer_.reset();
    }
}

// ---------------------------------------------------------------------------
// CacheManager
// ---------------------------------------------------------------------------

CacheManager::CacheManager(ClientContext &context) {
    (void)context;
}

CacheManager::~CacheManager() = default;

CacheManager &CacheManager::Get(ClientContext &context) {
    auto &state_manager = *context.registered_state;
    auto state = state_manager.GetOrCreate<CacheManager>("hbase_cache_manager", context);
    return *state;
}

CacheConfig CacheManager::GetConfig(ClientContext &context) const {
    return ReadCacheConfig(context);
}

void CacheManager::EnsureStorage(ClientContext &context, const CacheConfig &config) {
    if (storage_ready_ && directory_ == config.directory && mode_ == config.mode) {
        return;
    }
    auto &fs = FileSystem::GetFileSystem(context);
    // 先建存储（会创建目录），再打开元数据库
    if (config.mode == "memory") {
        storage_ = CreateMemoryCacheStorage();
    } else {
        storage_ = CreateFileCacheStorage(fs, config.directory);
    }
    metadata_ = make_uniq<CacheMetadataStore>(fs, config.directory);
    directory_ = config.directory;
    mode_ = config.mode;
    storage_ready_ = true;
    maintained_ = false;
}

bool CacheManager::MatchesStorageMode(const SegmentDescriptor &segment) const {
    if (mode_ == "memory") {
        return IsMemoryPath(segment.file_path);
    }
    return !IsMemoryPath(segment.file_path);
}

void CacheManager::Maintain(int64_t now_ms) {
    (void)now_ms;
    if (maintained_) {
        return;
    }
    maintained_ = true;
    // 崩溃恢复：staging 残留文件与未提交元数据一律不可见，直接清理
    storage_->CleanupStaging();
    for (auto &segment : metadata_->CleanupUncommitted()) {
        storage_->DeleteSegment(segment);
    }
    // 过期段不做物理删除：覆盖判定已排除它们，物理回收留给 hbase_cache_clear /
    // M5 淘汰（设计文档 §10），以便 hbase_cache_stats() 持续报告过期段数。
}

TableCacheStatistics &CacheManager::StatisticsFor(const string &namespace_name, const string &table) {
    auto key = StatisticsKey(namespace_name, table);
    auto entry = statistics_.find(key);
    if (entry == statistics_.end()) {
        TableCacheStatistics stats;
        stats.namespace_name = namespace_name;
        stats.table = table;
        entry = statistics_.emplace(key, std::move(stats)).first;
    }
    return entry->second;
}

CacheLookupResult CacheManager::Lookup(ClientContext &context, const ScanPlan &plan) {
    CacheLookupResult result;
    if (!plan.allow_cache) {
        result.reason = "cache disabled for this query (cache := false)";
        return result;
    }
    auto config = GetConfig(context);
    if (!config.enabled) {
        result.reason = "cache disabled by hbase_cache_enabled";
        return result;
    }
    if (!plan.consistency.IsCacheable()) {
        result.reason = "scan consistency is not cacheable";
        return result;
    }
    if (plan.include_stop) {
        result.reason = "stop_row is inclusive; half-open segments cannot represent it";
        return result;
    }

    auto now = CurrentTimeMillis();
    const lock_guard<mutex> guard(lock_);
    EnsureStorage(context, config);
    Maintain(now);

    vector<SegmentDescriptor> segments;
    metadata_->LoadSegments(plan.namespace_name, plan.table, segments);
    vector<SegmentDescriptor> usable;
    usable.reserve(segments.size());
    for (auto &segment : segments) {
        if (MatchesStorageMode(segment)) {
            usable.push_back(segment);
        }
    }

    result = ComputeCacheLookup(usable, plan, now);
    auto &stats = StatisticsFor(plan.namespace_name, plan.table);
    if (result.IsFullHit()) {
        stats.full_hit_count++;
    } else {
        stats.miss_count++;
    }
    return result;
}

unique_ptr<ICacheSegmentReader> CacheManager::OpenReader(const SegmentDescriptor &segment) {
    const lock_guard<mutex> guard(lock_);
    return storage_->OpenReader(segment);
}

unique_ptr<CacheWriteSession> CacheManager::BeginWrite(ClientContext &context, const ScanPlan &plan,
                                                       const RowKeyRange &partition_range) {
    if (!plan.allow_cache || !plan.consistency.IsCacheable() || plan.include_stop) {
        return nullptr;
    }
    auto config = GetConfig(context);
    if (!config.enabled) {
        return nullptr;
    }
    auto now = CurrentTimeMillis();
    const lock_guard<mutex> guard(lock_);
    try {
        EnsureStorage(context, config);
        Maintain(now);

        SegmentDescriptor segment;
        segment.segment_id = GenerateSegmentId();
        segment.namespace_name = plan.namespace_name;
        segment.table = plan.table;
        segment.range = partition_range;
        segment.created_at = now;
        segment.expires_at = config.ttl_seconds > 0 ? now + config.ttl_seconds * 1000 : 0;
        segment.generation = 1;
        segment.consistency = plan.consistency;
        segment.columns = plan.required_columns;
        segment.status = SegmentStatus::WRITING;
        segment.file_path = storage_->NewSegmentPath(segment.segment_id);

        auto writer = storage_->CreateWriter(segment);
        return make_uniq<CacheWriteSession>(*this, plan, std::move(segment), std::move(writer));
    } catch (std::exception &) {
        StatisticsFor(plan.namespace_name, plan.table).write_error_count++;
        return nullptr;
    }
}

void CacheManager::CommitSegment(SegmentDescriptor segment) {
    const lock_guard<mutex> guard(lock_);
    try {
        metadata_->CommitSegment(segment);
    } catch (...) {
        // 元数据提交失败：数据文件不得留下（否则成为未登记的孤儿文件）
        storage_->DeleteSegment(segment);
        throw;
    }
    auto &stats = StatisticsFor(segment.namespace_name, segment.table);
    stats.segments++;
    stats.rows += segment.row_count;
    stats.bytes += segment.file_size;
}

void CacheManager::MarkSegmentInvalid(const string &segment_id, const SegmentDescriptor &segment) {
    const lock_guard<mutex> guard(lock_);
    try {
        metadata_->MarkStatus(segment_id, SegmentStatus::INVALID);
        storage_->DeleteSegment(segment);
    } catch (std::exception &) {
        StatisticsFor(segment.namespace_name, segment.table).write_error_count++;
    }
}

void CacheManager::RecordInvalidatedHit(const ScanPlan &plan) {
    const lock_guard<mutex> guard(lock_);
    auto &stats = StatisticsFor(plan.namespace_name, plan.table);
    if (stats.full_hit_count > 0) {
        stats.full_hit_count--;
    }
    stats.miss_count++;
}

void CacheManager::RecordWriteError(const string &namespace_name, const string &table) {
    const lock_guard<mutex> guard(lock_);
    StatisticsFor(namespace_name, table).write_error_count++;
}

idx_t CacheManager::InvalidateTable(ClientContext &context, const string &namespace_name,
                                    const string &table) {
    auto config = GetConfig(context);
    const lock_guard<mutex> guard(lock_);
    EnsureStorage(context, config);
    Maintain(CurrentTimeMillis());
    auto segments = metadata_->DeleteSegmentsOfTable(namespace_name, table);
    for (auto &segment : segments) {
        storage_->DeleteSegment(segment);
    }
    auto entry = statistics_.find(StatisticsKey(namespace_name, table));
    if (entry != statistics_.end()) {
        entry->second.segments = 0;
        entry->second.rows = 0;
        entry->second.bytes = 0;
        entry->second.expired_segments = 0;
    }
    return segments.size();
}

idx_t CacheManager::InvalidateAll(ClientContext &context) {
    auto config = GetConfig(context);
    const lock_guard<mutex> guard(lock_);
    EnsureStorage(context, config);
    Maintain(CurrentTimeMillis());
    auto segments = metadata_->DeleteAllSegments();
    for (auto &segment : segments) {
        storage_->DeleteSegment(segment);
    }
    for (auto &entry : statistics_) {
        entry.second.segments = 0;
        entry.second.rows = 0;
        entry.second.bytes = 0;
        entry.second.expired_segments = 0;
    }
    storage_->CleanupStaging();
    return segments.size();
}

vector<TableCacheStatistics> CacheManager::GetStatistics(ClientContext &context) {
    auto config = GetConfig(context);
    const lock_guard<mutex> guard(lock_);
    EnsureStorage(context, config);

    auto now = CurrentTimeMillis();
    vector<TableCacheStatistics> result;
    unordered_map<string, idx_t> index_of;

    vector<SegmentDescriptor> segments;
    metadata_->LoadAllSegments(segments);
    for (auto &segment : segments) {
        if (!MatchesStorageMode(segment)) {
            continue;
        }
        auto key = StatisticsKey(segment.namespace_name, segment.table);
        auto entry = index_of.find(key);
        idx_t index;
        if (entry == index_of.end()) {
            index = result.size();
            index_of[key] = index;
            TableCacheStatistics stats;
            stats.namespace_name = segment.namespace_name;
            stats.table = segment.table;
            result.push_back(std::move(stats));
        } else {
            index = entry->second;
        }
        auto &stats = result[index];
        if (segment.status != SegmentStatus::COMMITTED) {
            continue;
        }
        if (segment.IsExpired(now)) {
            stats.expired_segments++;
            continue;
        }
        stats.segments++;
        stats.rows += segment.row_count;
        stats.bytes += segment.file_size;
    }

    // 合并会话内计数（命中 / 未命中 / 远端调用 / 写失败）
    for (auto &entry : statistics_) {
        auto &counters = entry.second;
        auto index = index_of.find(entry.first);
        if (index == index_of.end()) {
            index_of[entry.first] = result.size();
            result.push_back(counters);
            continue;
        }
        auto &stats = result[index->second];
        stats.full_hit_count = counters.full_hit_count;
        stats.partial_hit_count = counters.partial_hit_count;
        stats.miss_count = counters.miss_count;
        stats.remote_scan_count = counters.remote_scan_count;
        stats.remote_get_count = counters.remote_get_count;
        stats.write_error_count = counters.write_error_count;
    }
    return result;
}

void CacheManager::RecordRemoteScan(const string &namespace_name, const string &table) {
    const lock_guard<mutex> guard(lock_);
    StatisticsFor(namespace_name, table).remote_scan_count++;
}

void CacheManager::RecordRemoteGet(const string &namespace_name, const string &table) {
    const lock_guard<mutex> guard(lock_);
    StatisticsFor(namespace_name, table).remote_get_count++;
}

}  // namespace cache
}  // namespace hbase
}  // namespace duckdb
