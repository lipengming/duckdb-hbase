#include "cache/cache_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"

#include <cstring>

namespace duckdb {
namespace hbase {
namespace cache {

//! 段文件格式 v1（自研列式，见 docs/cache-design.md §7）：
//!
//!   [0, 8)          magic "DHBSEG01"
//!   重复的块：
//!     u32 rows | u32 payload_size | payload[payload_size] | u32 crc32(payload)
//!   footer：
//!     payload: u32 version | u32 block_rows | u64 row_count | u64 block_count
//!              | u32 column_count | 每列 {family, qualifier, logical_type}
//!     u32 crc32(footer_payload)
//!     u64 footer_payload_size   // 含 crc
//!     magic "DHBSEGF1"
//!
//! payload 布局（逐列）：u32 validity_len | validity | u32 lengths_len | lengths(rows×u32)
//!                      | u32 data_len | data
//!
//! 所有整数为小端。读端校验 magic、footer CRC、块 CRC、长度表与数据长度一致性，
//! 任何不一致都判定为损坏并抛 IOException（调用方按"缓存损坏 → 作废段并回源"处理）。

namespace {

constexpr const char *kSegmentMagic = "DHBSEG01";
constexpr const char *kFooterMagic = "DHBSEGF1";
constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kBlockRows = 2048;
constexpr idx_t kMaxBlockBytes = 256ULL * 1024 * 1024;

//! CRC32（IEEE 802.3，与 zlib/gzip 一致）。
uint32_t Crc32(const void *data, idx_t size) {
    static uint32_t table[256];
    static bool initialized = false;
    if (!initialized) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; bit++) {
                value = (value & 1) ? (0xEDB88320U ^ (value >> 1)) : (value >> 1);
            }
            table[i] = value;
        }
        initialized = true;
    }
    auto bytes = static_cast<const uint8_t *>(data);
    uint32_t crc = 0xFFFFFFFFU;
    for (idx_t i = 0; i < size; i++) {
        crc = table[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

void PutU32(string &buffer, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        buffer.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

void PutU64(string &buffer, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        buffer.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

void PutString(string &buffer, const string &value) {
    PutU32(buffer, static_cast<uint32_t>(value.size()));
    buffer.append(value);
}

class ByteReader {
public:
    ByteReader(const char *data, idx_t size) : data_(data), size_(size) {
    }

    uint32_t ReadU32() {
        Require(4);
        uint32_t value = 0;
        for (int i = 0; i < 4; i++) {
            value |= static_cast<uint32_t>(static_cast<uint8_t>(data_[offset_ + i])) << (8 * i);
        }
        offset_ += 4;
        return value;
    }

    uint64_t ReadU64() {
        Require(8);
        uint64_t value = 0;
        for (int i = 0; i < 8; i++) {
            value |= static_cast<uint64_t>(static_cast<uint8_t>(data_[offset_ + i])) << (8 * i);
        }
        offset_ += 8;
        return value;
    }

    string ReadString() {
        auto length = ReadU32();
        return string(ReadBytes(length), length);
    }

    const char *ReadBytes(idx_t length) {
        Require(length);
        auto pointer = data_ + offset_;
        offset_ += length;
        return pointer;
    }

private:
    void Require(idx_t length) {
        if (length > size_ - offset_) {
            throw IOException("hbase cache: 段文件被截断或损坏（需要 %llu 字节，剩余 %llu）",
                              static_cast<unsigned long long>(length),
                              static_cast<unsigned long long>(size_ - offset_));
        }
    }

    const char *data_;
    idx_t size_;
    idx_t offset_ = 0;
};

void WriteAll(FileHandle &handle, const string &data) {
    idx_t written = 0;
    while (written < data.size()) {
        auto length = MinValue<idx_t>(data.size() - written, 1ULL << 20);
        handle.Write(const_cast<char *>(data.data() + written), length);
        written += length;
    }
}

string ReadAt(FileHandle &handle, idx_t location, idx_t length) {
    string buffer;
    buffer.resize(length);
    handle.Read(&buffer[0], length, location);
    return buffer;
}

string JoinColumnNames(const vector<CacheColumn> &columns) {
    string result;
    for (idx_t i = 0; i < columns.size(); i++) {
        if (i > 0) {
            result += ", ";
        }
        result += columns[i].DisplayName();
    }
    return result;
}

void ValidateColumns(const vector<CacheColumn> &file_columns, const vector<CacheColumn> &expected,
                     const SegmentDescriptor &segment) {
    for (auto &column : expected) {
        bool found = false;
        for (auto &file_column : file_columns) {
            if (file_column == column) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw IOException("hbase cache: 段 %s 缺少列 %s（文件列: %s）", segment.segment_id,
                              column.DisplayName(), JoinColumnNames(file_columns));
        }
    }
}

struct SegmentFooter {
    uint32_t version = kFormatVersion;
    uint32_t block_rows = kBlockRows;
    uint64_t row_count = 0;
    uint64_t block_count = 0;
    vector<CacheColumn> columns;
};

class SegmentFileWriter : public ICacheSegmentWriter {
public:
    SegmentFileWriter(FileSystem &fs, string staging_path, string final_path, vector<CacheColumn> columns)
        : fs_(fs), staging_path_(std::move(staging_path)), final_path_(std::move(final_path)),
          columns_(std::move(columns)) {
        block_values_.resize(columns_.size());
        block_valid_.resize(columns_.size());
        handle_ = fs_.OpenFile(staging_path_,
                               FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
        string magic(kSegmentMagic);
        WriteAll(*handle_, magic);
    }

    void Append(idx_t row_count, const vector<vector<string>> &values,
                const vector<vector<bool>> &valid) override {
        if (!handle_) {
            throw IOException("hbase cache: 段写入器已关闭（%s）", staging_path_);
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
        }
        for (idx_t row = 0; row < row_count; row++) {
            for (idx_t column = 0; column < columns_.size(); column++) {
                block_values_[column].push_back(values[column][row]);
                block_valid_[column].push_back(valid[column][row]);
            }
            block_rows_++;
            row_count_++;
            if (block_rows_ >= kBlockRows) {
                FlushBlock();
            }
        }
    }

    idx_t RowCount() const override {
        return row_count_;
    }

    idx_t FileSize() const override {
        return file_size_;
    }

    string Finish() override {
        if (!handle_) {
            throw IOException("hbase cache: 段写入器已关闭（%s）", staging_path_);
        }
        FlushBlock();
        WriteFooter();
        handle_->Sync();
        file_size_ = handle_->GetFileSize();
        handle_->Close();
        handle_.reset();
        // 原子发布：同名目录内 rename，崩溃时要么看不到文件、要么看到完整文件
        fs_.MoveFile(staging_path_, final_path_);
        return final_path_;
    }

    void Abort() override {
        if (handle_) {
            try {
                handle_->Close();
            } catch (...) {  // NOLINT: 关闭失败不能覆盖原始错误
            }
            handle_.reset();
        }
        try {
            if (fs_.FileExists(staging_path_)) {
                fs_.RemoveFile(staging_path_);
            }
        } catch (...) {  // NOLINT: 清理失败不影响调用方
        }
    }

private:
    void FlushBlock() {
        if (block_rows_ == 0) {
            return;
        }
        string payload;
        for (idx_t column = 0; column < columns_.size(); column++) {
            idx_t validity_bytes = (block_rows_ + 7) / 8;
            string validity(validity_bytes, static_cast<char>(0));
            for (idx_t row = 0; row < block_rows_; row++) {
                if (block_valid_[column][row]) {
                    validity[row / 8] = static_cast<char>(static_cast<uint8_t>(validity[row / 8]) |
                                                          static_cast<uint8_t>(1U << (row % 8)));
                }
            }
            PutU32(payload, static_cast<uint32_t>(validity_bytes));
            payload.append(validity);

            string data;
            data.reserve(block_rows_ * 8);
            PutU32(payload, static_cast<uint32_t>(block_rows_ * 4));
            for (idx_t row = 0; row < block_rows_; row++) {
                if (!block_valid_[column][row]) {
                    PutU32(payload, 0);
                    continue;
                }
                auto &value = block_values_[column][row];
                PutU32(payload, static_cast<uint32_t>(value.size()));
                data.append(value);
            }
            PutU32(payload, static_cast<uint32_t>(data.size()));
            payload.append(data);
        }

        string block;
        PutU32(block, static_cast<uint32_t>(block_rows_));
        PutU32(block, static_cast<uint32_t>(payload.size()));
        block.append(payload);
        PutU32(block, Crc32(payload.data(), payload.size()));
        WriteAll(*handle_, block);

        block_count_++;
        block_rows_ = 0;
        for (idx_t column = 0; column < columns_.size(); column++) {
            block_values_[column].clear();
            block_valid_[column].clear();
        }
    }

    void WriteFooter() {
        string payload;
        PutU32(payload, kFormatVersion);
        PutU32(payload, kBlockRows);
        PutU64(payload, row_count_);
        PutU64(payload, block_count_);
        PutU32(payload, static_cast<uint32_t>(columns_.size()));
        for (auto &column : columns_) {
            PutString(payload, column.family);
            PutString(payload, column.qualifier);
            PutString(payload, column.logical_type);
        }
        string trailer;
        PutU32(trailer, Crc32(payload.data(), payload.size()));
        auto payload_size = static_cast<uint64_t>(payload.size() + trailer.size());
        PutU64(trailer, payload_size);

        WriteAll(*handle_, payload);
        WriteAll(*handle_, trailer);
        string magic(kFooterMagic);
        WriteAll(*handle_, magic);
    }

    FileSystem &fs_;
    string staging_path_;
    string final_path_;
    vector<CacheColumn> columns_;
    unique_ptr<FileHandle> handle_;
    vector<vector<string>> block_values_;
    vector<vector<bool>> block_valid_;
    idx_t block_rows_ = 0;
    idx_t row_count_ = 0;
    idx_t block_count_ = 0;
    idx_t file_size_ = 0;
};

class SegmentFileReader : public ICacheSegmentReader {
public:
    SegmentFileReader(FileSystem &fs, const SegmentDescriptor &segment,
                      const vector<CacheColumn> &request_columns)
        : segment_(segment) {
        handle_ = fs.OpenFile(segment.file_path, FileFlags::FILE_FLAGS_READ);
        auto file_size = static_cast<idx_t>(handle_->GetFileSize());
        // 最小文件：前置 magic(8) + footer payload 至少 24 + crc(4) + size(8) + magic(8)
        if (file_size < 40) {
            throw IOException("hbase cache: 段 %s 文件过小(%llu 字节)，判定为损坏", segment.segment_id,
                              static_cast<unsigned long long>(file_size));
        }
        if (segment.file_size > 0 && static_cast<idx_t>(file_size) != segment.file_size) {
            throw IOException("hbase cache: 段 %s 文件大小(%llu)与元数据(%llu)不一致，判定为损坏",
                              segment.segment_id, static_cast<unsigned long long>(file_size),
                              static_cast<unsigned long long>(segment.file_size));
        }
        if (ReadAt(*handle_, 0, 8) != kSegmentMagic) {
            throw IOException("hbase cache: 段 %s 魔数不匹配，判定为损坏", segment.segment_id);
        }
        if (ReadAt(*handle_, file_size - 8, 8) != kFooterMagic) {
            throw IOException("hbase cache: 段 %s footer 魔数不匹配，判定为损坏", segment.segment_id);
        }
        auto footer_size_bytes = ReadAt(*handle_, file_size - 16, 8);
        ByteReader size_reader(footer_size_bytes.data(), footer_size_bytes.size());
        auto footer_size = size_reader.ReadU64();
        if (footer_size < 4 || footer_size + 24 > static_cast<uint64_t>(file_size)) {
            throw IOException("hbase cache: 段 %s footer 长度非法(%llu)", segment.segment_id,
                              static_cast<unsigned long long>(footer_size));
        }
        auto footer_offset = file_size - 16 - static_cast<idx_t>(footer_size);
        auto footer = ReadAt(*handle_, footer_offset, static_cast<idx_t>(footer_size));
        auto payload_size = footer.size() - 4;
        ByteReader crc_reader(footer.data() + payload_size, 4);
        auto stored_crc = crc_reader.ReadU32();
        if (stored_crc != Crc32(footer.data(), payload_size)) {
            throw IOException("hbase cache: 段 %s footer 校验失败，判定为损坏", segment.segment_id);
        }
        ByteReader reader(footer.data(), payload_size);
        footer_.version = reader.ReadU32();
        footer_.block_rows = reader.ReadU32();
        footer_.row_count = reader.ReadU64();
        footer_.block_count = reader.ReadU64();
        auto column_count = reader.ReadU32();
        for (uint32_t i = 0; i < column_count; i++) {
            CacheColumn column;
            column.family = reader.ReadString();
            column.qualifier = reader.ReadString();
            column.logical_type = reader.ReadString();
            footer_.columns.push_back(std::move(column));
        }
        if (footer_.version != kFormatVersion) {
            throw IOException("hbase cache: 段 %s 格式版本 %u 不受支持（期望 %u）", segment.segment_id,
                              footer_.version, kFormatVersion);
        }
        if (footer_.block_rows == 0 || footer_.block_rows > kBlockRows) {
            throw IOException("hbase cache: 段 %s block_rows 非法(%u)，判定为损坏", segment.segment_id,
                              footer_.block_rows);
        }
        ValidateColumns(footer_.columns, request_columns, segment);
        data_offset_ = 8;
        // 打开即整段校验：逐块比对 CRC，确认块数/行数与 footer 一致。
        // 代价是一次顺序读（命中路径的 I/O 翻倍），换取"绝不会读到一半才发现损坏"：
        // 损坏段在 InitGlobal 就被识别并回源，而不是让查询中途报错。
        ValidateIntegrity(footer_offset);
    }

    const vector<CacheColumn> &Columns() const override {
        return footer_.columns;
    }

    idx_t RowCount() const override {
        return footer_.row_count;
    }

    bool Next(CacheBatch &batch) override {
        batch = CacheBatch();
        if (rows_read_ >= footer_.row_count) {
            return false;
        }
        auto block_header = ReadAt(*handle_, data_offset_, 8);
        ByteReader header_reader(block_header.data(), block_header.size());
        auto block_rows = header_reader.ReadU32();
        auto payload_size = header_reader.ReadU32();
        if (block_rows == 0 || block_rows > footer_.block_rows) {
            throw IOException("hbase cache: 段 %s 块行数非法(%u)，判定为损坏", segment_.segment_id, block_rows);
        }
        if (payload_size == 0 || payload_size > kMaxBlockBytes) {
            throw IOException("hbase cache: 段 %s 块长度非法(%u)，判定为损坏", segment_.segment_id, payload_size);
        }
        if (rows_read_ + block_rows > footer_.row_count) {
            throw IOException("hbase cache: 段 %s 块行数超出 footer 记录的总行数", segment_.segment_id);
        }
        auto payload = ReadAt(*handle_, data_offset_ + 8, payload_size);
        auto crc_bytes = ReadAt(*handle_, data_offset_ + 8 + payload_size, 4);
        ByteReader crc_reader(crc_bytes.data(), 4);
        if (crc_reader.ReadU32() != Crc32(payload.data(), payload.size())) {
            throw IOException("hbase cache: 段 %s 块校验失败，判定为损坏", segment_.segment_id);
        }

        ByteReader reader(payload.data(), payload.size());
        batch.columns.resize(footer_.columns.size());
        batch.valid.resize(footer_.columns.size());
        for (idx_t column = 0; column < footer_.columns.size(); column++) {
            auto validity_len = reader.ReadU32();
            auto validity = reader.ReadBytes(validity_len);
            auto lengths_len = reader.ReadU32();
            if (lengths_len != block_rows * 4) {
                throw IOException("hbase cache: 段 %s 长度表非法(%u != %u)，判定为损坏", segment_.segment_id,
                                  lengths_len, block_rows * 4);
            }
            auto lengths = reader.ReadBytes(lengths_len);
            auto data_len = reader.ReadU32();
            auto data = reader.ReadBytes(data_len);

            auto &values = batch.columns[column];
            auto &valid = batch.valid[column];
            values.reserve(block_rows);
            valid.reserve(block_rows);
            idx_t cursor = 0;
            ByteReader length_reader(lengths, lengths_len);
            for (uint32_t row = 0; row < block_rows; row++) {
                auto length = length_reader.ReadU32();
                bool row_valid =
                    (static_cast<uint8_t>(validity[row / 8]) & static_cast<uint8_t>(1U << (row % 8))) != 0;
                if (!row_valid) {
                    valid.push_back(false);
                    values.emplace_back();
                    continue;
                }
                if (cursor + length > data_len) {
                    throw IOException("hbase cache: 段 %s 列数据越界，判定为损坏", segment_.segment_id);
                }
                valid.push_back(true);
                values.emplace_back(data + cursor, length);
                cursor += length;
            }
            if (cursor != data_len) {
                throw IOException("hbase cache: 段 %s 列数据长度不一致(%llu != %u)，判定为损坏",
                                  segment_.segment_id, static_cast<unsigned long long>(cursor), data_len);
            }
        }
        data_offset_ += 8 + payload_size + 4;
        rows_read_ += block_rows;
        batch.row_count = block_rows;
        return true;
    }

private:
    //! 顺序校验全部块：块头合法、CRC 匹配、块数/行数与 footer 一致。
    void ValidateIntegrity(idx_t footer_offset) {
        idx_t offset = 8;
        idx_t rows = 0;
        idx_t blocks = 0;
        while (offset < footer_offset) {
            if (offset + 8 > footer_offset) {
                throw IOException("hbase cache: 段 %s 块头越过 footer，判定为损坏", segment_.segment_id);
            }
            auto header_bytes = ReadAt(*handle_, offset, 8);
            ByteReader header(header_bytes.data(), header_bytes.size());
            auto block_rows = header.ReadU32();
            auto payload_size = header.ReadU32();
            if (block_rows == 0 || block_rows > footer_.block_rows || payload_size == 0 ||
                payload_size > kMaxBlockBytes) {
                throw IOException("hbase cache: 段 %s 块头非法，判定为损坏", segment_.segment_id);
            }
            if (offset + 8 + payload_size + 4 > footer_offset) {
                throw IOException("hbase cache: 段 %s 块长度越过 footer，判定为损坏", segment_.segment_id);
            }
            auto payload = ReadAt(*handle_, offset + 8, payload_size);
            auto crc_bytes = ReadAt(*handle_, offset + 8 + payload_size, 4);
            ByteReader crc_reader(crc_bytes.data(), crc_bytes.size());
            if (crc_reader.ReadU32() != Crc32(payload.data(), payload.size())) {
                throw IOException("hbase cache: 段 %s 块校验失败，判定为损坏", segment_.segment_id);
            }
            rows += block_rows;
            blocks++;
            offset += 8 + payload_size + 4;
        }
        if (offset != footer_offset) {
            throw IOException("hbase cache: 段 %s 块数据与 footer 边界不一致，判定为损坏", segment_.segment_id);
        }
        if (rows != footer_.row_count || blocks != footer_.block_count) {
            throw IOException(
                "hbase cache: 段 %s 行数/块数与 footer 记录不一致(%llu/%llu vs %llu/%llu)，判定为损坏",
                segment_.segment_id, static_cast<unsigned long long>(rows),
                static_cast<unsigned long long>(blocks),
                static_cast<unsigned long long>(footer_.row_count),
                static_cast<unsigned long long>(footer_.block_count));
        }
    }

    SegmentDescriptor segment_;
    SegmentFooter footer_;
    unique_ptr<FileHandle> handle_;
    idx_t data_offset_ = 0;
    idx_t rows_read_ = 0;
};

class SegmentFileStorage : public ICacheStorage {
public:
    SegmentFileStorage(FileSystem &fs, string directory) : fs_(fs), directory_(std::move(directory)) {
        segments_directory_ = fs_.JoinPath(directory_, "segments");
        staging_directory_ = fs_.JoinPath(directory_, "staging");
        EnsureDirectory(directory_);
        EnsureDirectory(segments_directory_);
        EnsureDirectory(staging_directory_);
    }

    string NewSegmentPath(const string &segment_id) override {
        return fs_.JoinPath(segments_directory_, segment_id + ".seg");
    }

    unique_ptr<ICacheSegmentWriter> CreateWriter(const SegmentDescriptor &segment) override {
        if (segment.file_path.empty()) {
            throw IOException("hbase cache: 段 %s 缺少目标文件路径", segment.segment_id);
        }
        auto staging = fs_.JoinPath(staging_directory_, segment.segment_id + ".tmp");
        return make_uniq<SegmentFileWriter>(fs_, staging, segment.file_path, segment.columns);
    }

    unique_ptr<ICacheSegmentReader> OpenReader(const SegmentDescriptor &segment) override {
        if (!fs_.FileExists(segment.file_path)) {
            throw IOException("hbase cache: 段 %s 文件不存在（%s）", segment.segment_id, segment.file_path);
        }
        return make_uniq<SegmentFileReader>(fs_, segment, segment.columns);
    }

    bool DeleteSegment(const SegmentDescriptor &segment) override {
        try {
            if (!segment.file_path.empty() && fs_.FileExists(segment.file_path)) {
                fs_.RemoveFile(segment.file_path);
            }
            auto staging = fs_.JoinPath(staging_directory_, segment.segment_id + ".tmp");
            if (fs_.FileExists(staging)) {
                fs_.RemoveFile(staging);
            }
            return true;
        } catch (...) {  // NOLINT: 删除失败不影响查询
            return false;
        }
    }

    void CleanupStaging() override {
        try {
            for (auto &entry : fs_.Glob(fs_.JoinPath(staging_directory_, "*"))) {
                try {
                    fs_.RemoveFile(entry.path);
                } catch (...) {  // NOLINT
                }
            }
        } catch (...) {  // NOLINT: staging 清理是尽力而为
        }
    }

private:
    void EnsureDirectory(const string &path) {
        if (!fs_.DirectoryExists(path)) {
            fs_.CreateDirectory(path);
        }
    }

    FileSystem &fs_;
    string directory_;
    string segments_directory_;
    string staging_directory_;
};

}  // namespace

unique_ptr<ICacheStorage> CreateFileCacheStorage(FileSystem &fs, const string &directory) {
    return make_uniq<SegmentFileStorage>(fs, directory);
}

}  // namespace cache
}  // namespace hbase
}  // namespace duckdb
