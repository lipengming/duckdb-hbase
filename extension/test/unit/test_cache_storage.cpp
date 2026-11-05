// 段存储单测：文件格式往返、NULL/空串/二进制字节、损坏检测、staging 清理；内存存储等价性。
#include "cache/cache_storage.hpp"

#include "catch.hpp"
#include "duckdb/common/file_system.hpp"

#include <filesystem>

using namespace duckdb;
using namespace duckdb::hbase;
using namespace duckdb::hbase::cache;

namespace {

//! 每个用例独立的临时目录，析构时递归删除。
class TestDirectory {
public:
    explicit TestDirectory(const string &name) {
        auto base = std::filesystem::temp_directory_path();
        path = (base / ("hbase_unit_" + name + "_" + std::to_string(counter_++))).string();
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

vector<CacheColumn> TestColumns() {
    return {CacheColumn{"cf", "name", "VARCHAR"}, CacheColumn{"cf", "note", "VARCHAR"},
            CacheColumn{"cf", "raw", "VARCHAR"}};
}

SegmentDescriptor TestSegment(const string &directory, const string &id) {
    SegmentDescriptor segment;
    segment.segment_id = id;
    segment.namespace_name = "default";
    segment.table = "t";
    segment.range.start = "a";
    segment.range.stop = "z";
    segment.columns = TestColumns();
    segment.file_path = directory + "/segments/" + id + ".seg";
    return segment;
}

CacheBatch MakeBatch(const vector<vector<string>> &values, const vector<vector<bool>> &valid) {
    CacheBatch batch;
    batch.row_count = values.empty() ? 0 : values[0].size();
    batch.columns = values;
    batch.valid = valid;
    return batch;
}

}  // namespace

TEST_CASE("文件段写入后可完整读回（跨多个块）", "[cache][storage]") {
    TestDirectory directory("file_roundtrip");
    auto fs = FileSystem::CreateLocal();
    auto storage = CreateFileCacheStorage(*fs, directory.path);
    auto segment = TestSegment(directory.path, "seg1");

    const idx_t row_count = 5000;  // 跨越 kBlockRows(2048) 的多个块
    vector<vector<string>> values(3);
    vector<vector<bool>> valid(3);
    for (idx_t row = 0; row < row_count; row++) {
        values[0].push_back("name-" + std::to_string(row));
        valid[0].push_back(true);
        if (row % 7 == 0) {
            values[1].push_back(string());  // 空串 != NULL
            valid[1].push_back(true);
        } else if (row % 5 == 0) {
            values[1].push_back(string());  // NULL
            valid[1].push_back(false);
        } else {
            values[1].push_back("note-" + std::to_string(row));
            valid[1].push_back(true);
        }
        // 含 0x00 / 0xFF 的二进制字节不得被 UTF-8 语义改写
        string raw;
        raw.push_back(static_cast<char>(row % 256));
        raw.push_back('\0');
        raw.push_back(static_cast<char>(0xFF));
        values[2].push_back(raw);
        valid[2].push_back(true);
    }

    {
        auto writer = storage->CreateWriter(segment);
        // 分多批追加，模拟流式写入
        for (idx_t offset = 0; offset < row_count; offset += 512) {
            auto take = MinValue<idx_t>(512, row_count - offset);
            vector<vector<string>> batch_values(3);
            vector<vector<bool>> batch_valid(3);
            for (idx_t column = 0; column < 3; column++) {
                batch_values[column].assign(values[column].begin() + static_cast<int64_t>(offset),
                                            values[column].begin() + static_cast<int64_t>(offset + take));
                batch_valid[column].assign(valid[column].begin() + static_cast<int64_t>(offset),
                                           valid[column].begin() + static_cast<int64_t>(offset + take));
            }
            writer->Append(take, batch_values, batch_valid);
        }
        REQUIRE(writer->RowCount() == row_count);
        auto path = writer->Finish();
        REQUIRE(path == segment.file_path);
        REQUIRE(writer->FileSize() > 0);
    }

    REQUIRE(fs->FileExists(segment.file_path));
    // staging 不应残留
    REQUIRE(fs->Glob(directory.path + "/staging/*").empty());

    auto reader = storage->OpenReader(segment);
    REQUIRE(reader->RowCount() == row_count);
    REQUIRE(reader->Columns().size() == 3);

    idx_t read_rows = 0;
    CacheBatch batch;
    while (reader->Next(batch)) {
        REQUIRE(batch.columns.size() == 3);
        for (idx_t row = 0; row < batch.row_count; row++) {
            auto index = read_rows + row;
            REQUIRE(batch.columns[0][row] == values[0][index]);
            REQUIRE(batch.valid[0][row] == true);
            REQUIRE(batch.valid[1][row] == valid[1][index]);
            if (valid[1][index]) {
                REQUIRE(batch.columns[1][row] == values[1][index]);
            }
            REQUIRE(batch.columns[2][row] == values[2][index]);
        }
        read_rows += batch.row_count;
    }
    REQUIRE(read_rows == row_count);
}

TEST_CASE("空段（0 行）可以写入并读回", "[cache][storage]") {
    TestDirectory directory("empty_segment");
    auto fs = FileSystem::CreateLocal();
    auto storage = CreateFileCacheStorage(*fs, directory.path);
    auto segment = TestSegment(directory.path, "empty");

    auto writer = storage->CreateWriter(segment);
    writer->Finish();

    auto reader = storage->OpenReader(segment);
    REQUIRE(reader->RowCount() == 0);
    CacheBatch batch;
    REQUIRE_FALSE(reader->Next(batch));
}

TEST_CASE("截断的段文件被判定为损坏", "[cache][storage]") {
    TestDirectory directory("corrupt_truncate");
    auto fs = FileSystem::CreateLocal();
    auto storage = CreateFileCacheStorage(*fs, directory.path);
    auto segment = TestSegment(directory.path, "corrupt");

    {
        auto writer = storage->CreateWriter(segment);
        vector<vector<string>> values {{"a"}, {"b"}, {"c"}};
        vector<vector<bool>> valid {{true}, {true}, {true}};
        writer->Append(1, values, valid);
        writer->Finish();
    }

    auto size = static_cast<int64_t>(std::filesystem::file_size(segment.file_path));
    REQUIRE(size > 32);
    {
        auto handle = fs->OpenFile(segment.file_path, FileFlags::FILE_FLAGS_WRITE);
        handle->Truncate(size - 12);
        handle->Close();
    }

    REQUIRE_THROWS_AS(storage->OpenReader(segment), IOException);
}

TEST_CASE("段文件内容被改写时校验失败", "[cache][storage]") {
    TestDirectory directory("corrupt_bitflip");
    auto fs = FileSystem::CreateLocal();
    auto storage = CreateFileCacheStorage(*fs, directory.path);
    auto segment = TestSegment(directory.path, "bitflip");

    {
        auto writer = storage->CreateWriter(segment);
        vector<vector<string>> values {{"hello"}, {"world"}, {"raw"}};
        vector<vector<bool>> valid {{true}, {true}, {true}};
        writer->Append(1, values, valid);
        writer->Finish();
    }

    // 改写第一个块的数据区（跳过 magic + 块头）
    {
        auto handle = fs->OpenFile(segment.file_path, FileFlags::FILE_FLAGS_WRITE);
        char garbage = static_cast<char>(0x5A);
        handle->Seek(20);
        handle->Write(&garbage, 1);
        handle->Close();
    }

    // 打开即整段校验：损坏必须在 OpenReader 阶段被发现，绝不留给"读到一半"
    REQUIRE_THROWS_AS(storage->OpenReader(segment), IOException);
}

TEST_CASE("缺失的段文件打开即失败", "[cache][storage]") {
    TestDirectory directory("missing");
    auto fs = FileSystem::CreateLocal();
    auto storage = CreateFileCacheStorage(*fs, directory.path);
    auto segment = TestSegment(directory.path, "missing");
    REQUIRE_THROWS_AS(storage->OpenReader(segment), IOException);
}

TEST_CASE("删除段文件与 staging 残留清理", "[cache][storage]") {
    TestDirectory directory("delete");
    auto fs = FileSystem::CreateLocal();
    auto storage = CreateFileCacheStorage(*fs, directory.path);
    auto segment = TestSegment(directory.path, "to_delete");

    {
        auto writer = storage->CreateWriter(segment);
        vector<vector<string>> values {{"a"}, {"b"}, {"c"}};
        vector<vector<bool>> valid {{true}, {true}, {true}};
        writer->Append(1, values, valid);
        writer->Finish();
    }
    REQUIRE(fs->FileExists(segment.file_path));
    REQUIRE(storage->DeleteSegment(segment));
    REQUIRE_FALSE(fs->FileExists(segment.file_path));

    // 模拟崩溃残留：staging 目录里有半成品文件
    auto staging = directory.path + "/staging/leftover.tmp";
    {
        auto flags = FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW;
        auto handle = fs->OpenFile(staging, flags);
        handle->Write(const_cast<char *>("half"), 4);
        handle->Close();
    }
    REQUIRE(fs->FileExists(staging));
    storage->CleanupStaging();
    REQUIRE_FALSE(fs->FileExists(staging));
}

TEST_CASE("内存存储与文件存储行为一致", "[cache][storage]") {
    auto storage = CreateMemoryCacheStorage();
    SegmentDescriptor segment;
    segment.segment_id = "mem1";
    segment.columns = TestColumns();
    segment.file_path = storage->NewSegmentPath(segment.segment_id);

    auto writer = storage->CreateWriter(segment);
    vector<vector<string>> values {{"a", "b", "c"}, {"x", "", "z"}, {"1", "2", "3"}};
    vector<vector<bool>> valid {{true, true, true}, {true, false, true}, {true, true, true}};
    writer->Append(3, values, valid);
    REQUIRE(writer->RowCount() == 3);
    writer->Finish();

    auto reader = storage->OpenReader(segment);
    REQUIRE(reader->RowCount() == 3);
    CacheBatch batch;
    REQUIRE(reader->Next(batch));
    REQUIRE(batch.row_count == 3);
    REQUIRE(batch.columns[0][1] == "b");
    REQUIRE(batch.valid[1][1] == false);
    REQUIRE(batch.columns[1][0] == "x");
    REQUIRE(batch.columns[1][1] == "");

    REQUIRE(storage->DeleteSegment(segment));
    REQUIRE_THROWS_AS(storage->OpenReader(segment), IOException);
}

TEST_CASE("写入器 Abort 后不留下文件", "[cache][storage]") {
    TestDirectory directory("abort");
    auto fs = FileSystem::CreateLocal();
    auto storage = CreateFileCacheStorage(*fs, directory.path);
    auto segment = TestSegment(directory.path, "aborted");

    {
        auto writer = storage->CreateWriter(segment);
        vector<vector<string>> values {{"a"}, {"b"}, {"c"}};
        vector<vector<bool>> valid {{true}, {true}, {true}};
        writer->Append(1, values, valid);
        writer->Abort();
    }
    REQUIRE_FALSE(fs->FileExists(segment.file_path));
    REQUIRE(fs->Glob(directory.path + "/staging/*").empty());
}
