#pragma once

#include "cache/cache_types.hpp"

namespace duckdb {
namespace hbase {
namespace cache {

//! 缓存读写使用的批次视图：列式、UTF8（当前数据面统一 VARCHAR），validity 逐行标记。
struct CacheBatch {
    idx_t row_count = 0;
    vector<vector<string>> columns;
    vector<vector<bool>> valid;
};

//! 段写入器：只管数据文件，不碰元数据。元数据由 CacheManager 在 Finish 后提交。
class ICacheSegmentWriter {
public:
    virtual ~ICacheSegmentWriter() = default;

    //! 追加一批行（values[column][row]，valid 同形）。列布局必须与 CreateWriter 时一致。
    //! 直接接收列式视图，避免把远端解码结果再拷一层。
    virtual void Append(idx_t row_count, const vector<vector<string>> &values,
                        const vector<vector<bool>> &valid) = 0;
    virtual idx_t RowCount() const = 0;
    //! 已完成写入的字节数（用于统计）。
    virtual idx_t FileSize() const = 0;
    //! 完成写入：fsync 后原子发布，返回最终可读路径。失败抛 IOException。
    virtual string Finish() = 0;
    //! 放弃写入并清理临时文件（幂等，不抛异常）。
    virtual void Abort() = 0;
};

//! 段读取器：流式读取，内存占用与段行数无关。
class ICacheSegmentReader {
public:
    virtual ~ICacheSegmentReader() = default;

    virtual const vector<CacheColumn> &Columns() const = 0;
    virtual idx_t RowCount() const = 0;
    //! 读取下一批；返回 false 表示读完。数据损坏抛 IOException。
    virtual bool Next(CacheBatch &batch) = 0;
};

//! 段存储：数据文件的 CreateWriter / OpenReader / DeleteSegment。
//! 元数据（Segment 索引）不在这一层，见 CacheMetadataStore。
class ICacheStorage {
public:
    virtual ~ICacheStorage() = default;

    //! 段数据文件的最终路径（file: <root>/segments/<id>.seg；memory: memory://<id>）。
    virtual string NewSegmentPath(const string &segment_id) = 0;
    virtual unique_ptr<ICacheSegmentWriter> CreateWriter(const SegmentDescriptor &segment) = 0;
    virtual unique_ptr<ICacheSegmentReader> OpenReader(const SegmentDescriptor &segment) = 0;
    virtual bool DeleteSegment(const SegmentDescriptor &segment) = 0;
    //! 清理崩溃残留的 staging 文件（只清 staging 目录，不碰已发布段）。
    virtual void CleanupStaging() = 0;
};

//! 落盘存储：<root>/segments/<segment_id>.seg，写入先落 <root>/staging 再 rename。
unique_ptr<ICacheStorage> CreateFileCacheStorage(FileSystem &fs, const string &directory);

//! 进程内存储（hbase_cache_mode='memory'，同时用于单测）。
unique_ptr<ICacheStorage> CreateMemoryCacheStorage();

}  // namespace cache
}  // namespace hbase
}  // namespace duckdb
