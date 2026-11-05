#include "cache/cache_metadata_store.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"

#include "sqlite3.h"

#include <functional>

namespace duckdb {
namespace hbase {
namespace cache {

namespace {

//! 元数据表结构（对应方案 §2.7 建议表；namespace/table_name 由 table_id 拆分而来，见 docs/cache-design.md）。
const char *kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS cache_segment (
    segment_id     TEXT PRIMARY KEY,
    namespace      TEXT NOT NULL,
    table_name     TEXT NOT NULL,
    start_row      BLOB NOT NULL,
    stop_row       BLOB NOT NULL,
    row_count      INTEGER NOT NULL,
    created_at     INTEGER NOT NULL,
    expires_at     INTEGER NOT NULL,
    generation     INTEGER NOT NULL,
    file_path      TEXT NOT NULL,
    file_size      INTEGER NOT NULL,
    status         TEXT NOT NULL,
    max_versions   INTEGER NOT NULL,
    timestamp_min  INTEGER NOT NULL,
    timestamp_max  INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_cache_segment_table ON cache_segment(namespace, table_name, status);
CREATE TABLE IF NOT EXISTS cache_segment_column (
    segment_id   TEXT NOT NULL,
    family       TEXT NOT NULL,
    qualifier    TEXT NOT NULL,
    logical_type TEXT NOT NULL,
    PRIMARY KEY (segment_id, family, qualifier)
);
)SQL";

const char *kSegmentColumns =
    "segment_id, namespace, table_name, start_row, stop_row, row_count, created_at, expires_at, "
    "generation, file_path, file_size, status, max_versions, timestamp_min, timestamp_max";

string BlobToString(sqlite3_stmt *statement, int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return string();
    }
    auto data = static_cast<const char *>(sqlite3_column_blob(statement, column));
    auto size = sqlite3_column_bytes(statement, column);
    return data == nullptr ? string() : string(data, static_cast<idx_t>(size));
}

string ColumnString(sqlite3_stmt *statement, int column) {
    auto data = reinterpret_cast<const char *>(sqlite3_column_text(statement, column));
    auto size = sqlite3_column_bytes(statement, column);
    return data == nullptr ? string() : string(data, static_cast<idx_t>(size));
}

int64_t ColumnInt64(sqlite3_stmt *statement, int column) {
    return sqlite3_column_int64(statement, column);
}

//! RAII 包装：编译一次、必 finalize。
class SqliteStatement {
public:
    SqliteStatement(sqlite3 *db, const string &sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement_, nullptr) != SQLITE_OK) {
            throw IOException("hbase cache: 元数据语句编译失败: %s", sqlite3_errmsg(db));
        }
    }

    ~SqliteStatement() {
        if (statement_) {
            sqlite3_finalize(statement_);
        }
    }

    SqliteStatement(const SqliteStatement &) = delete;
    SqliteStatement &operator=(const SqliteStatement &) = delete;

    void BindBlob(int index, const string &value) {
        // 空 RowKey 表示该侧无界，存零长度 BLOB 而非 NULL
        auto rc = sqlite3_bind_blob(statement_, index, value.data(), static_cast<int>(value.size()),
                                    SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) {
            throw IOException("hbase cache: 绑定 BLOB 参数失败: %s", sqlite3_errmsg(db_));
        }
    }

    void BindText(int index, const string &value) {
        auto rc = sqlite3_bind_text(statement_, index, value.c_str(), static_cast<int>(value.size()),
                                    SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) {
            throw IOException("hbase cache: 绑定文本参数失败: %s", sqlite3_errmsg(db_));
        }
    }

    void BindInt64(int index, int64_t value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            throw IOException("hbase cache: 绑定整数参数失败: %s", sqlite3_errmsg(db_));
        }
    }

    //! 执行一步；返回是否有结果行。
    bool Step() {
        auto rc = sqlite3_step(statement_);
        if (rc == SQLITE_ROW) {
            return true;
        }
        if (rc == SQLITE_DONE) {
            return false;
        }
        throw IOException("hbase cache: 元数据语句执行失败: %s", sqlite3_errmsg(db_));
    }

    void Execute() {
        while (Step()) {
        }
    }

    //! SQLite 要求复用语句前先 reset，否则再次 bind 会返回 SQLITE_MISUSE。
    void Reset() {
        sqlite3_reset(statement_);
        sqlite3_clear_bindings(statement_);
    }

    sqlite3_stmt *Get() {
        return statement_;
    }

private:
    sqlite3 *db_;
    sqlite3_stmt *statement_ = nullptr;
};

SegmentDescriptor ReadSegment(sqlite3_stmt *statement) {
    SegmentDescriptor segment;
    segment.segment_id = ColumnString(statement, 0);
    segment.namespace_name = ColumnString(statement, 1);
    segment.table = ColumnString(statement, 2);
    segment.range.start = BlobToString(statement, 3);
    segment.range.stop = BlobToString(statement, 4);
    segment.row_count = static_cast<idx_t>(ColumnInt64(statement, 5));
    segment.created_at = ColumnInt64(statement, 6);
    segment.expires_at = ColumnInt64(statement, 7);
    segment.generation = static_cast<idx_t>(ColumnInt64(statement, 8));
    segment.file_path = ColumnString(statement, 9);
    segment.file_size = static_cast<idx_t>(ColumnInt64(statement, 10));
    SegmentStatus status;
    if (!TryParseSegmentStatus(ColumnString(statement, 11), status)) {
        status = SegmentStatus::INVALID;
    }
    segment.status = status;
    segment.consistency.max_versions = static_cast<int32_t>(ColumnInt64(statement, 12));
    segment.consistency.timestamp_min = ColumnInt64(statement, 13);
    segment.consistency.timestamp_max = ColumnInt64(statement, 14);
    return segment;
}

//! 执行一条段查询并用 binder 绑定参数；随后一次性补齐列清单。
vector<SegmentDescriptor> QuerySegments(sqlite3 *db, const string &sql,
                                        const std::function<void(SqliteStatement &)> &binder);

vector<SegmentDescriptor> QuerySegments(sqlite3 *db, const string &sql,
                                        const std::function<void(SqliteStatement &)> &binder) {
    vector<SegmentDescriptor> segments;
    SqliteStatement statement(db, sql);
    binder(statement);
    while (statement.Step()) {
        segments.push_back(ReadSegment(statement.Get()));
    }
    if (segments.empty()) {
        return segments;
    }

    // 一次取回所有列，避免 N+1 查询
    unordered_map<string, idx_t> index_of;
    string placeholders;
    for (idx_t i = 0; i < segments.size(); i++) {
        index_of[segments[i].segment_id] = i;
        if (i > 0) {
            placeholders += ", ";
        }
        placeholders += "?";
    }
    SqliteStatement columns(db, "SELECT segment_id, family, qualifier, logical_type "
                                "FROM cache_segment_column WHERE segment_id IN (" +
                                    placeholders + ") ORDER BY segment_id, family, qualifier");
    for (idx_t i = 0; i < segments.size(); i++) {
        columns.BindText(static_cast<int>(i + 1), segments[i].segment_id);
    }
    while (columns.Step()) {
        auto entry = index_of.find(ColumnString(columns.Get(), 0));
        if (entry == index_of.end()) {
            continue;
        }
        CacheColumn column;
        column.family = ColumnString(columns.Get(), 1);
        column.qualifier = ColumnString(columns.Get(), 2);
        column.logical_type = ColumnString(columns.Get(), 3);
        segments[entry->second].columns.push_back(std::move(column));
    }
    return segments;
}

}  // namespace

CacheMetadataStore::CacheMetadataStore(FileSystem &fs, const string &directory)
    : directory_(directory) {
    Open(fs);
    CreateSchema();
}

CacheMetadataStore::~CacheMetadataStore() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void CacheMetadataStore::Open(FileSystem &fs) {
    if (!fs.DirectoryExists(directory_)) {
        fs.CreateDirectory(directory_);
    }
    auto path = DatabasePath();
    auto rc = sqlite3_open_v2(
        path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        auto message = db_ ? string(sqlite3_errmsg(db_)) : string("unknown error");
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw IOException("hbase cache: 打开元数据数据库失败（%s）: %s", path, message);
    }
    // WAL + FULL 同步：提交后崩溃不丢元数据（方案 §7.6 失败恢复）
    char *error = nullptr;
    auto exec_rc = sqlite3_exec(db_,
                                "PRAGMA journal_mode=WAL;"
                                "PRAGMA synchronous=FULL;"
                                "PRAGMA busy_timeout=5000;",
                                nullptr, nullptr, &error);
    if (exec_rc != SQLITE_OK) {
        auto message = error ? string(error) : string(sqlite3_errmsg(db_));
        sqlite3_free(error);
        throw IOException("hbase cache: 初始化元数据数据库失败: %s", message);
    }
}

void CacheMetadataStore::CreateSchema() {
    const lock_guard<mutex> guard(lock_);
    char *error = nullptr;
    if (sqlite3_exec(db_, kSchemaSql, nullptr, nullptr, &error) != SQLITE_OK) {
        auto message = error ? string(error) : string(sqlite3_errmsg(db_));
        sqlite3_free(error);
        throw IOException("hbase cache: 创建元数据表失败: %s", message);
    }
    if (sqlite3_exec(db_, "PRAGMA user_version=1;", nullptr, nullptr, &error) != SQLITE_OK) {
        auto message = error ? string(error) : string(sqlite3_errmsg(db_));
        sqlite3_free(error);
        throw IOException("hbase cache: 设置元数据版本失败: %s", message);
    }
}

void CacheMetadataStore::LoadSegments(const string &namespace_name, const string &table,
                                      vector<SegmentDescriptor> &out) {
    const lock_guard<mutex> guard(lock_);
    out = QuerySegments(db_, string("SELECT ") + kSegmentColumns + " FROM cache_segment "
                        "WHERE namespace = ? AND table_name = ? ORDER BY created_at",
                        [&](SqliteStatement &statement) {
                            statement.BindText(1, namespace_name);
                            statement.BindText(2, table);
                        });
}

void CacheMetadataStore::LoadAllSegments(vector<SegmentDescriptor> &out) {
    const lock_guard<mutex> guard(lock_);
    out = QuerySegments(db_,
                        string("SELECT ") + kSegmentColumns + " FROM cache_segment ORDER BY created_at",
                        [](SqliteStatement &) {});
}

void CacheMetadataStore::CommitSegment(const SegmentDescriptor &segment) {
    const lock_guard<mutex> guard(lock_);
    char *error = nullptr;
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &error) != SQLITE_OK) {
        auto message = error ? string(error) : string(sqlite3_errmsg(db_));
        sqlite3_free(error);
        throw IOException("hbase cache: 开启元数据事务失败: %s", message);
    }
    try {
        SqliteStatement insert(db_, string("INSERT INTO cache_segment (") + kSegmentColumns +
                                       ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        insert.BindText(1, segment.segment_id);
        insert.BindText(2, segment.namespace_name);
        insert.BindText(3, segment.table);
        insert.BindBlob(4, segment.range.start);
        insert.BindBlob(5, segment.range.stop);
        insert.BindInt64(6, static_cast<int64_t>(segment.row_count));
        insert.BindInt64(7, segment.created_at);
        insert.BindInt64(8, segment.expires_at);
        insert.BindInt64(9, static_cast<int64_t>(segment.generation));
        insert.BindText(10, segment.file_path);
        insert.BindInt64(11, static_cast<int64_t>(segment.file_size));
        // 正常写入路径只提交 COMMITTED；其它状态用于崩溃恢复演练与排障
        insert.BindText(12, SegmentStatusName(segment.status));
        insert.BindInt64(13, segment.consistency.max_versions);
        insert.BindInt64(14, segment.consistency.timestamp_min);
        insert.BindInt64(15, segment.consistency.timestamp_max);
        insert.Execute();

        SqliteStatement column_statement(
            db_, "INSERT OR REPLACE INTO cache_segment_column (segment_id, family, qualifier, "
                 "logical_type) VALUES (?, ?, ?, ?)");
        for (auto &column : segment.columns) {
            column_statement.Reset();
            column_statement.BindText(1, segment.segment_id);
            column_statement.BindText(2, column.family);
            column_statement.BindText(3, column.qualifier);
            column_statement.BindText(4, column.logical_type);
            column_statement.Execute();
        }

        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &error) != SQLITE_OK) {
            auto message = error ? string(error) : string(sqlite3_errmsg(db_));
            sqlite3_free(error);
            throw IOException("hbase cache: 提交元数据事务失败: %s", message);
        }
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

vector<SegmentDescriptor> CacheMetadataStore::DeleteSegmentsOfTable(const string &namespace_name,
                                                                   const string &table) {
    const lock_guard<mutex> guard(lock_);
    auto segments = QuerySegments(db_, string("SELECT ") + kSegmentColumns + " FROM cache_segment "
                                  "WHERE namespace = ? AND table_name = ? ORDER BY created_at",
                                  [&](SqliteStatement &statement) {
                                      statement.BindText(1, namespace_name);
                                      statement.BindText(2, table);
                                  });
    SqliteStatement delete_columns(
        db_, "DELETE FROM cache_segment_column WHERE segment_id IN "
             "(SELECT segment_id FROM cache_segment WHERE namespace = ? AND table_name = ?)");
    delete_columns.BindText(1, namespace_name);
    delete_columns.BindText(2, table);
    delete_columns.Execute();

    SqliteStatement delete_segments(db_,
                                    "DELETE FROM cache_segment WHERE namespace = ? AND table_name = ?");
    delete_segments.BindText(1, namespace_name);
    delete_segments.BindText(2, table);
    delete_segments.Execute();
    return segments;
}

vector<SegmentDescriptor> CacheMetadataStore::DeleteAllSegments() {
    const lock_guard<mutex> guard(lock_);
    auto segments =
        QuerySegments(db_,
                      string("SELECT ") + kSegmentColumns + " FROM cache_segment ORDER BY created_at",
                      [](SqliteStatement &) {});
    SqliteStatement delete_columns(db_, "DELETE FROM cache_segment_column");
    delete_columns.Execute();
    SqliteStatement delete_segments(db_, "DELETE FROM cache_segment");
    delete_segments.Execute();
    return segments;
}

void CacheMetadataStore::MarkStatus(const string &segment_id, SegmentStatus status) {
    const lock_guard<mutex> guard(lock_);
    SqliteStatement statement(db_, "UPDATE cache_segment SET status = ? WHERE segment_id = ?");
    statement.BindText(1, SegmentStatusName(status));
    statement.BindText(2, segment_id);
    statement.Execute();
}

vector<SegmentDescriptor> CacheMetadataStore::CleanupUncommitted() {
    const lock_guard<mutex> guard(lock_);
    auto segments = QuerySegments(db_, string("SELECT ") + kSegmentColumns + " FROM cache_segment "
                                  "WHERE status <> 'COMMITTED' ORDER BY created_at",
                                  [](SqliteStatement &) {});
    if (!segments.empty()) {
        SqliteStatement delete_columns(
            db_, "DELETE FROM cache_segment_column WHERE segment_id IN "
                 "(SELECT segment_id FROM cache_segment WHERE status <> 'COMMITTED')");
        delete_columns.Execute();
        SqliteStatement delete_segments(db_,
                                        "DELETE FROM cache_segment WHERE status <> 'COMMITTED'");
        delete_segments.Execute();
    }
    return segments;
}

}  // namespace cache
}  // namespace hbase
}  // namespace duckdb
