#include "sql/hbase_cache_functions.hpp"

#include "cache/cache_manager.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {
namespace hbase {

namespace {

using cache::CacheManager;
using cache::TableCacheStatistics;

void WriteBigInt(DataChunk &output, idx_t column, idx_t row, int64_t value) {
    FlatVector::GetDataMutable<int64_t>(output.data[column])[row] = value;
}

void WriteString(DataChunk &output, idx_t column, idx_t row, const string &value) {
    FlatVector::GetDataMutable<string_t>(output.data[column])[row] =
        StringVector::AddString(output.data[column], value);
}

// ---------------------------------------------------------------------------
// hbase_cache_stats()
// ---------------------------------------------------------------------------

struct CacheStatsGlobalState : public GlobalTableFunctionState {
    vector<TableCacheStatistics> rows;
    bool loaded = false;
};

unique_ptr<GlobalTableFunctionState> CacheStatsInitGlobal(ClientContext &context,
                                                          TableFunctionInitInput &input) {
    auto state = make_uniq<CacheStatsGlobalState>();
    state->rows = CacheManager::Get(context).GetStatistics(context);
    state->loaded = true;
    return std::move(state);
}

void CacheStatsFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    auto &state = data.global_state->Cast<CacheStatsGlobalState>();
    output.Reset();
    if (!state.loaded) {
        return;
    }
    idx_t row = 0;
    for (auto &stat : state.rows) {
        if (row >= STANDARD_VECTOR_SIZE) {
            break;
        }
        WriteString(output, 0, row, stat.namespace_name);
        WriteString(output, 1, row, stat.table);
        WriteBigInt(output, 2, row, static_cast<int64_t>(stat.segments));
        WriteBigInt(output, 3, row, static_cast<int64_t>(stat.rows));
        WriteBigInt(output, 4, row, static_cast<int64_t>(stat.bytes));
        WriteBigInt(output, 5, row, static_cast<int64_t>(stat.expired_segments));
        WriteBigInt(output, 6, row, static_cast<int64_t>(stat.full_hit_count));
        WriteBigInt(output, 7, row, static_cast<int64_t>(stat.partial_hit_count));
        WriteBigInt(output, 8, row, static_cast<int64_t>(stat.miss_count));
        WriteBigInt(output, 9, row, static_cast<int64_t>(stat.remote_scan_count));
        WriteBigInt(output, 10, row, static_cast<int64_t>(stat.remote_get_count));
        WriteBigInt(output, 11, row, static_cast<int64_t>(stat.write_error_count));
        row++;
    }
    // 统计一次性输出：剩余行留在 state 里会重复输出，这里直接清空
    state.rows.clear();
    state.loaded = false;
    output.SetChildCardinality(row);
}

unique_ptr<FunctionData> CacheStatsBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<Identifier> &names) {
    auto add = [&](const char *name, LogicalType type) {
        names.push_back(Identifier(name));
        return_types.push_back(std::move(type));
    };
    add("namespace", LogicalType::VARCHAR);
    add("table_name", LogicalType::VARCHAR);
    add("segments", LogicalType::BIGINT);
    add("rows", LogicalType::BIGINT);
    add("bytes", LogicalType::BIGINT);
    add("expired_segments", LogicalType::BIGINT);
    add("full_hit_count", LogicalType::BIGINT);
    add("partial_hit_count", LogicalType::BIGINT);
    add("miss_count", LogicalType::BIGINT);
    add("remote_scan_count", LogicalType::BIGINT);
    add("remote_get_count", LogicalType::BIGINT);
    add("write_error_count", LogicalType::BIGINT);
    return make_uniq<TableFunctionData>();
}

// ---------------------------------------------------------------------------
// hbase_cache_clear([connection, ] [namespace, table])
// ---------------------------------------------------------------------------

struct CacheClearBindData : public TableFunctionData {
    bool clear_all = true;
    string namespace_name;
    string table;
};

unique_ptr<FunctionData> CacheClearBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<Identifier> &names) {
    auto result = make_uniq<CacheClearBindData>();
    names.push_back(Identifier("removed_segments"));
    return_types.push_back(LogicalType::BIGINT);
    if (input.inputs.size() == 2) {
        result->clear_all = false;
        result->namespace_name = StringValue::Get(input.inputs[0]);
        result->table = StringValue::Get(input.inputs[1]);
    } else if (input.inputs.size() == 3) {
        // 与方案 §4.2 示例一致：hbase_cache_clear('prod','default','customer_profile')
        result->clear_all = false;
        result->namespace_name = StringValue::Get(input.inputs[1]);
        result->table = StringValue::Get(input.inputs[2]);
    }
    return std::move(result);
}

struct CacheClearGlobalState : public GlobalTableFunctionState {
    idx_t removed = 0;
    bool emitted = false;
};

unique_ptr<GlobalTableFunctionState> CacheClearInitGlobal(ClientContext &context,
                                                          TableFunctionInitInput &input) {
    auto state = make_uniq<CacheClearGlobalState>();
    auto &bind_data = input.bind_data->Cast<CacheClearBindData>();
    auto &manager = CacheManager::Get(context);
    if (bind_data.clear_all) {
        state->removed = manager.InvalidateAll(context);
    } else {
        state->removed = manager.InvalidateTable(context, bind_data.namespace_name, bind_data.table);
    }
    return std::move(state);
}

void CacheClearFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    auto &state = data.global_state->Cast<CacheClearGlobalState>();
    output.Reset();
    if (state.emitted) {
        output.SetChildCardinality(0);
        return;
    }
    state.emitted = true;
    WriteBigInt(output, 0, 0, static_cast<int64_t>(state.removed));
    output.SetChildCardinality(1);
}

}  // namespace

void RegisterCacheFunctions(ExtensionLoader &loader) {
    TableFunction stats("hbase_cache_stats", {}, CacheStatsFunction, CacheStatsBind,
                        CacheStatsInitGlobal);
    loader.RegisterFunction(stats);

    // 无参：清理全部
    TableFunction clear_all("hbase_cache_clear", {}, CacheClearFunction, CacheClearBind,
                            CacheClearInitGlobal);
    loader.RegisterFunction(clear_all);

    // (namespace, table)：清理单表
    TableFunction clear_table("hbase_cache_clear", {LogicalType::VARCHAR, LogicalType::VARCHAR},
                              CacheClearFunction, CacheClearBind, CacheClearInitGlobal);
    loader.RegisterFunction(clear_table);

    // (connection, namespace, table)：与方案 §4.2 示例一致；缓存按进程本地存储，连接名仅用于兼容签名
    TableFunction clear_connection_table(
        "hbase_cache_clear", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
        CacheClearFunction, CacheClearBind, CacheClearInitGlobal);
    loader.RegisterFunction(clear_connection_table);
}

}  // namespace hbase
}  // namespace duckdb
