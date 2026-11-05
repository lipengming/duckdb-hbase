#include "cache/cache_storage.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {
namespace hbase {
namespace cache {

namespace {

constexpr idx_t kMemoryBatchRows = 2048;

//! 进程内段数据。memory 模式下数据不落盘，进程退出即失效（I1：缓存属于单个进程）。
struct MemorySegmentData {
    vector<CacheColumn> columns;
    vector<vector<string>> values;
    vector<vector<bool>> valid;
    idx_t row_count = 0;
};

//! 进程级注册表：多个 CacheManager（多个 DuckDB 连接）共享同一份内存段。
class MemorySegmentRegistry {
public:
    static MemorySegmentRegistry &Get() {
        static MemorySegmentRegistry instance;
        return instance;
    }

    mutex lock;
    unordered_map<string, shared_ptr<MemorySegmentData>> segments;
};

class MemorySegmentWriter : public ICacheSegmentWriter {
public:
    explicit MemorySegmentWriter(vector<CacheColumn> columns) : columns_(std::move(columns)) {
        data_ = make_shared_ptr<MemorySegmentData>();
        data_->columns = columns_;
        data_->values.resize(columns_.size());
        data_->valid.resize(columns_.size());
    }

    void Append(idx_t row_count, const vector<vector<string>> &values,
                const vector<vector<bool>> &valid) override {
        if (finished_) {
            throw IOException("hbase cache: 内存段写入器已结束");
        }
        if (row_count == 0) {
            return;
        }
        if (values.size() != columns_.size() || valid.size() != columns_.size()) {
            throw IOException("hbase cache: 追加批次列数(%llu)与段列数(%llu)不一致",
                              static_cast<unsigned long long>(values.size()),
                              static_cast<unsigned long long>(columns_.size()));
        }
        for (idx_t column = 0; column < columns_.size(); column++) {
            if (values[column].size() != row_count || valid[column].size() != row_count) {
                throw IOException("hbase cache: 列 %s 的行数(%llu)与批次行数(%llu)不一致",
                                  columns_[column].DisplayName(),
                                  static_cast<unsigned long long>(values[column].size()),
                                  static_cast<unsigned long long>(row_count));
            }
            data_->values[column].insert(data_->values[column].end(), values[column].begin(),
                                         values[column].end());
            data_->valid[column].insert(data_->valid[column].end(), valid[column].begin(),
                                        valid[column].end());
        }
        data_->row_count += row_count;
        row_count_ += row_count;
    }

    idx_t RowCount() const override {
        return row_count_;
    }

    idx_t FileSize() const override {
        idx_t size = 0;
        for (auto &column : data_->values) {
            for (auto &value : column) {
                size += value.size();
            }
        }
        return size;
    }

    string Finish() override {
        finished_ = true;
        auto &registry = MemorySegmentRegistry::Get();
        const lock_guard<mutex> guard(registry.lock);
        registry.segments[segment_id_] = data_;
        return "memory://" + segment_id_;
    }

    void Abort() override {
        finished_ = true;
        data_ = nullptr;
    }

    void SetSegmentId(const string &segment_id) {
        segment_id_ = segment_id;
    }

private:
    vector<CacheColumn> columns_;
    shared_ptr<MemorySegmentData> data_;
    string segment_id_;
    idx_t row_count_ = 0;
    bool finished_ = false;
};

class MemorySegmentReader : public ICacheSegmentReader {
public:
    explicit MemorySegmentReader(shared_ptr<MemorySegmentData> data) : data_(std::move(data)) {
        if (!data_) {
            throw IOException("hbase cache: 内存段不存在");
        }
    }

    const vector<CacheColumn> &Columns() const override {
        return data_->columns;
    }

    idx_t RowCount() const override {
        return data_->row_count;
    }

    bool Next(CacheBatch &batch) override {
        batch = CacheBatch();
        if (offset_ >= data_->row_count) {
            return false;
        }
        auto take = MinValue<idx_t>(kMemoryBatchRows, data_->row_count - offset_);
        batch.columns.resize(data_->columns.size());
        batch.valid.resize(data_->columns.size());
        for (idx_t column = 0; column < data_->columns.size(); column++) {
            batch.columns[column].assign(
                data_->values[column].begin() + static_cast<int64_t>(offset_),
                data_->values[column].begin() + static_cast<int64_t>(offset_ + take));
            batch.valid[column].assign(data_->valid[column].begin() + static_cast<int64_t>(offset_),
                                       data_->valid[column].begin() + static_cast<int64_t>(offset_ + take));
        }
        batch.row_count = take;
        offset_ += take;
        return true;
    }

private:
    shared_ptr<MemorySegmentData> data_;
    idx_t offset_ = 0;
};

class MemoryCacheStorage : public ICacheStorage {
public:
    string NewSegmentPath(const string &segment_id) override {
        return "memory://" + segment_id;
    }

    unique_ptr<ICacheSegmentWriter> CreateWriter(const SegmentDescriptor &segment) override {
        auto writer = make_uniq<MemorySegmentWriter>(segment.columns);
        writer->SetSegmentId(segment.segment_id);
        return std::move(writer);
    }

    unique_ptr<ICacheSegmentReader> OpenReader(const SegmentDescriptor &segment) override {
        auto &registry = MemorySegmentRegistry::Get();
        shared_ptr<MemorySegmentData> data;
        {
            const lock_guard<mutex> guard(registry.lock);
            auto entry = registry.segments.find(segment.segment_id);
            if (entry == registry.segments.end()) {
                throw IOException("hbase cache: 内存段 %s 不存在", segment.segment_id);
            }
            data = entry->second;
        }
        return make_uniq<MemorySegmentReader>(std::move(data));
    }

    bool DeleteSegment(const SegmentDescriptor &segment) override {
        auto &registry = MemorySegmentRegistry::Get();
        const lock_guard<mutex> guard(registry.lock);
        return registry.segments.erase(segment.segment_id) > 0;
    }

    void CleanupStaging() override {
    }
};

}  // namespace

unique_ptr<ICacheStorage> CreateMemoryCacheStorage() {
    return make_uniq<MemoryCacheStorage>();
}

}  // namespace cache
}  // namespace hbase
}  // namespace duckdb
