#pragma once

#include "cache/cache_types.hpp"

struct sqlite3;

namespace duckdb {
namespace hbase {
namespace cache {

//! 段元数据的权威索引（SQLite）。
//!
//! 与数据文件的关系（见 docs/cache-design.md §7-§8）：
//!   - 数据先写 staging（不写元数据）→ 校验 → rename 发布 → 单事务写元数据；
//!   - 只有登记在 cache_segment 且 status=COMMITTED 的段才可见；
//!   - 崩溃后未提交记录由 CleanupUncommitted 清理，未登记文件由 CacheManager 回收。
//!
//! 线程安全：内部用互斥锁串行化所有 SQL（同一连接），可从多个扫描线程调用。
class CacheMetadataStore {
public:
    //! 打开（必要时创建）<directory>/metadata.db，并确保表结构存在。
    CacheMetadataStore(FileSystem &fs, const string &directory);
    ~CacheMetadataStore();

    CacheMetadataStore(const CacheMetadataStore &) = delete;
    CacheMetadataStore &operator=(const CacheMetadataStore &) = delete;

    string DatabasePath() const {
        return directory_ + "/metadata.db";
    }

    //! 读取单表全部段（含非 COMMITTED，由调用方按状态过滤）。
    void LoadSegments(const string &namespace_name, const string &table, vector<SegmentDescriptor> &out);
    //! 读取全部段（用于统计与全库清理）。
    void LoadAllSegments(vector<SegmentDescriptor> &out);
    //! 单事务提交：写入段行与列清单。失败抛 IOException，调用方需回滚数据文件。
    void CommitSegment(const SegmentDescriptor &segment);
    //! 删除单表 / 全库的段记录，返回被删除的描述符（调用方负责删数据文件）。
    vector<SegmentDescriptor> DeleteSegmentsOfTable(const string &namespace_name, const string &table);
    vector<SegmentDescriptor> DeleteAllSegments();
    //! 把段标记为不可用（保留排障信息）。
    void MarkStatus(const string &segment_id, SegmentStatus status);
    //! 崩溃恢复：删除所有非 COMMITTED 记录，返回被清理的段。
    vector<SegmentDescriptor> CleanupUncommitted();

private:
    void Open(FileSystem &fs);
    void CreateSchema();

    string directory_;
    sqlite3 *db_ = nullptr;
    mutable mutex lock_;
};

}  // namespace cache
}  // namespace hbase
}  // namespace duckdb
