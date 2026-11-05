#pragma once

#include "cache/cache_metadata_store.hpp"
#include "cache/cache_storage.hpp"
#include "cache/cache_types.hpp"
#include "duckdb/main/client_context_state.hpp"

namespace duckdb {
namespace hbase {
namespace cache {

class CacheManager;

//! 一次扫描分区（或单段）的写会话。
//!
//! 生命周期：BeginWrite → Append × N → Finish（提交）/ Abort（放弃）。
//! 析构时若仍未提交，自动 Abort：LIMIT、取消、异常都不会留下半成品段。
class CacheWriteSession {
public:
    CacheWriteSession(CacheManager &manager, ScanPlan plan, SegmentDescriptor segment,
                      unique_ptr<ICacheSegmentWriter> writer);
    ~CacheWriteSession();

    CacheWriteSession(const CacheWriteSession &) = delete;
    CacheWriteSession &operator=(const CacheWriteSession &) = delete;

    void Append(idx_t row_count, const vector<vector<string>> &values,
                const vector<vector<bool>> &valid);
    //! 发布数据文件并原子提交元数据。失败只记录统计并清理，不向查询抛错。
    bool Finish();
    //! 放弃写入并清理临时文件（幂等）。
    void Abort();

    idx_t RowCount() const {
        return segment_.row_count;
    }
    const ScanPlan &Plan() const {
        return plan_;
    }

private:
    CacheManager &manager_;
    ScanPlan plan_;
    SegmentDescriptor segment_;
    unique_ptr<ICacheSegmentWriter> writer_;
    bool finished_ = false;
};

//! 缓存门面，是 scan 层唯一入口（docs/cache-design.md §3）。
//!
//! 线程安全：内部用互斥锁串行化元数据读写与统计计数；可被多个扫描线程并发调用。
//! 实例通过 ClientContextState 绑定到 ClientContext，随会话销毁自动释放。
class CacheManager : public ClientContextState {
public:
    explicit CacheManager(ClientContext &context);
    ~CacheManager() override;

    static CacheManager &Get(ClientContext &context);

    CacheConfig GetConfig(ClientContext &context) const;

    //! 命中判定。缓存关闭或不可用时返回 MISS（reason 说明原因），不抛异常。
    CacheLookupResult Lookup(ClientContext &context, const ScanPlan &plan);
    //! 打开命中段；失败抛 IOException（调用方按"缓存损坏 → 作废段并回源"处理）。
    unique_ptr<ICacheSegmentReader> OpenReader(const SegmentDescriptor &segment);
    //! 为一个扫描分区开写会话；缓存不可用或准备失败时返回 nullptr（并计入 write_error）。
    unique_ptr<CacheWriteSession> BeginWrite(ClientContext &context, const ScanPlan &plan,
                                             const RowKeyRange &partition_range);

    //! 提交会话写入的段（由 CacheWriteSession 调用）。
    void CommitSegment(SegmentDescriptor segment);
    //! 段不可用：标记 INVALID 并删除数据文件（缓存损坏时调用）。
    void MarkSegmentInvalid(const string &segment_id, const SegmentDescriptor &segment);
    //! 命中段在打开阶段被判定损坏、整体回源：把这次 Lookup 记为 Miss（命中计数不可信）。
    void RecordInvalidatedHit(const ScanPlan &plan);
    //! 记录一次写缓存失败（查询仍正确，但失败必须可观测）。
    void RecordWriteError(const string &namespace_name, const string &table);

    idx_t InvalidateTable(ClientContext &context, const string &namespace_name, const string &table);
    idx_t InvalidateAll(ClientContext &context);

    vector<TableCacheStatistics> GetStatistics(ClientContext &context);

    void RecordRemoteScan(const string &namespace_name, const string &table);
    void RecordRemoteGet(const string &namespace_name, const string &table);

private:
    //! 按当前 SET 配置准备元数据存储与段存储；目录/模式变化时重建。
    void EnsureStorage(ClientContext &context, const CacheConfig &config);
    //! 首次使用时清理 staging 残留与未提交元数据，并回收过期段。
    void Maintain(int64_t now_ms);
    //! 段文件是否属于当前存储模式（file / memory 混用同一目录时不串味）。
    bool MatchesStorageMode(const SegmentDescriptor &segment) const;
    TableCacheStatistics &StatisticsFor(const string &namespace_name, const string &table);

    mutable mutex lock_;
    string directory_;
    string mode_;
    bool storage_ready_ = false;
    bool maintained_ = false;
    unique_ptr<CacheMetadataStore> metadata_;
    unique_ptr<ICacheStorage> storage_;
    unordered_map<string, TableCacheStatistics> statistics_;
};

}  // namespace cache
}  // namespace hbase
}  // namespace duckdb
