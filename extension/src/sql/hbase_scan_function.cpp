#include "sql/hbase_scan_function.hpp"

#include "common/connection_registry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "provider/remote_scan_provider.hpp"
#include "scan/columnar_batch_decoder.hpp"
#include "scan/remote_scan_stream.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace duckdb {
namespace hbase {

namespace {

//! Bind 阶段解析出的扫描参数与已解析的列集合。
struct HbaseScanBindData : public TableFunctionData {
    ConnectionConfig connection;
    std::shared_ptr<IRemoteScanProvider> provider;
    RemoteScanRequest request;
};

//! 执行期状态：后台流 + 当前已解码批次。
struct HbaseScanGlobalState : public GlobalTableFunctionState {
    unique_ptr<RemoteScanStream> stream;
    DecodedBatch batch;
    idx_t offset = 0;
    vector<column_t> column_ids;
    idx_t bind_column_count = 0;

    // 串行消费：即使 DuckDB 调度多个 Execute，也保证 batch/offset 不被并发修改
    std::mutex exec_lock;

    idx_t MaxThreads() const override {
        // M1：单个远端流串行消费；并行扫描在 M2 按 RowKey 区间切分。
        return 1;
    }
};

//! 把 Provider 返回的逻辑类型名映射为 DuckDB LogicalType；未知类型退化为 VARCHAR。
LogicalType MapSqlType(const string& sql_type) {
    auto upper = StringUtil::Upper(sql_type);
    if (upper == "BIGINT") {
        return LogicalType::BIGINT;
    }
    if (upper == "INTEGER" || upper == "INT") {
        return LogicalType::INTEGER;
    }
    if (upper == "DOUBLE") {
        return LogicalType::DOUBLE;
    }
    if (upper == "BOOLEAN") {
        return LogicalType::BOOLEAN;
    }
    if (upper == "BLOB") {
        return LogicalType::BLOB;
    }
    return LogicalType::VARCHAR;
}

bool IsValidColumnSpec(const string& spec) {
    auto sep = spec.find(':');
    if (sep == string::npos || sep == 0) {
        return false;
    }
    // qualifier 允许为空（表示整列族），但 family 不能为空
    return spec.find(':', sep + 1) == string::npos;
}

RemoteColumn ParseColumnSpec(const string& spec) {
    auto sep = spec.find(':');
    return RemoteColumn{spec.substr(0, sep), spec.substr(sep + 1), "VARCHAR"};
}

//! RowKey 参数既支持 VARCHAR 也支持 BLOB，两者都以原始字节保存。
string RowKeyValue(const Value& value) {
    if (value.IsNull()) {
        return string();
    }
    return StringValue::Get(value);
}

void ParseScanOptions(TableFunctionBindInput& input, HbaseScanBindData& result,
                      vector<string>& column_specs) {
    auto& request = result.request;
    for (auto& kv : input.named_parameters) {
        auto& key = kv.first;
        auto& value = kv.second;

        if (key == "columns") {
            if (value.IsNull()) {
                continue;
            }
            for (auto& child : ListValue::GetChildren(value)) {
                if (child.IsNull()) {
                    throw BinderException("hbase_scan: columns 列表不能包含 NULL");
                }
                auto spec = StringValue::Get(child);
                if (!IsValidColumnSpec(spec)) {
                    throw BinderException("hbase_scan: 列描述 '%s' 非法，期望 family:qualifier",
                                          spec);
                }
                column_specs.push_back(std::move(spec));
            }
        } else if (key == "start_row") {
            request.start_row = RowKeyValue(value);
        } else if (key == "stop_row") {
            request.stop_row = RowKeyValue(value);
        } else if (key == "max_versions") {
            if (!value.IsNull()) {
                request.max_versions = IntegerValue::Get(value);
                if (request.max_versions < 1) {
                    throw BinderException("hbase_scan: max_versions 必须 >= 1");
                }
            }
        } else if (key == "timestamp_min") {
            if (!value.IsNull()) {
                request.timestamp_min = BigIntValue::Get(value);
            }
        } else if (key == "timestamp_max") {
            if (!value.IsNull()) {
                request.timestamp_max = BigIntValue::Get(value);
            }
        } else if (key == "cache") {
            // 缓存引擎在 M3 接入；当前仅解析参数
            if (!value.IsNull()) {
                BooleanValue::Get(value);
            }
        } else {
            throw BinderException("hbase_scan: 未知参数 '%s'", key.GetIdentifierName());
        }
    }

    if (request.timestamp_min > 0 && request.timestamp_max > 0 &&
        request.timestamp_min > request.timestamp_max) {
        throw BinderException("hbase_scan: timestamp_min(%lld) 不能大于 timestamp_max(%lld)",
                              request.timestamp_min, request.timestamp_max);
    }
}

unique_ptr<FunctionData> HbaseScanBind(ClientContext& context, TableFunctionBindInput& input,
                                       vector<LogicalType>& return_types,
                                       vector<Identifier>& names) {
    auto result = make_uniq<HbaseScanBindData>();

    auto connection_name = StringValue::Get(input.inputs[0]);
    auto& registry = GetConnectionRegistry(context);
    ConnectionEntry entry;
    if (!registry.TryGet(connection_name, entry)) {
        throw BinderException(
            "hbase_scan: 连接 '%s' 未注册，请先执行 SELECT hbase_connect('%s', 'grpc://host:port')",
            connection_name, connection_name);
    }

    result->connection = entry.config;
    result->provider = entry.provider;
    result->request.namespace_name = StringValue::Get(input.inputs[1]);
    result->request.table = StringValue::Get(input.inputs[2]);
    result->request.request_id = "scan-" + connection_name + "-" + result->request.table;
    if (result->request.table.empty()) {
        throw BinderException("hbase_scan: 表名不能为空");
    }

    vector<string> column_specs;
    ParseScanOptions(input, *result, column_specs);

    if (!column_specs.empty()) {
        // 显式列：类型默认 VARCHAR（HBase 无类型信息，类型映射规则待定）
        for (auto& spec : column_specs) {
            result->request.columns.push_back(ParseColumnSpec(spec));
        }
    } else if (result->provider) {
        auto schema =
            result->provider->GetSchema(result->request.namespace_name, result->request.table);
        result->request.columns = schema.columns;
    }

    if (result->request.columns.empty()) {
        throw BinderException(
            "hbase_scan: 表 '%s.%s' 没有可用列，请显式指定 columns := ['cf:qualifier']",
            result->request.namespace_name, result->request.table);
    }

    for (auto& column : result->request.columns) {
        auto name =
            column.qualifier.empty() ? column.family : column.family + ":" + column.qualifier;
        names.push_back(Identifier(name));
        return_types.push_back(MapSqlType(column.sql_type));
    }

    return std::move(result);
}

unique_ptr<GlobalTableFunctionState> HbaseScanInitGlobal(ClientContext& context,
                                                         TableFunctionInitInput& input) {
    auto& bind_data = input.bind_data->Cast<HbaseScanBindData>();
    auto state = make_uniq<HbaseScanGlobalState>();
    state->column_ids = input.column_ids;
    state->bind_column_count = bind_data.request.columns.size();
    auto provider = bind_data.provider;
    auto request = bind_data.request;
    // 后台线程按批拉取，主线程按需消费；有界队列保证内存不随结果集增长
    state->stream =
        make_uniq<RemoteScanStream>([provider, request](const RemoteBatchCallback& callback) {
            provider->Scan(request, callback);
        });
    return std::move(state);
}

void HbaseScanExecute(ClientContext& context, TableFunctionInput& data, DataChunk& output) {
    auto& state = data.global_state->Cast<HbaseScanGlobalState>();
    std::lock_guard<std::mutex> guard(state.exec_lock);
    idx_t produced = 0;

    while (produced < STANDARD_VECTOR_SIZE) {
        if (state.offset >= state.batch.row_count) {
            string payload;
            int64_t row_count = 0;
            if (!state.stream->Next(payload, row_count)) {
                break;
            }
            state.batch = DecodeColumnarBatch(payload);
            state.offset = 0;
            if (state.batch.row_count == 0) {
                continue;
            }
            if (state.batch.columns.size() != state.bind_column_count) {
                throw IOException("hbase_scan: 数据批列数(%llu)与绑定列数(%llu)不一致",
                                  static_cast<unsigned long long>(state.batch.columns.size()),
                                  static_cast<unsigned long long>(state.bind_column_count));
            }
        }

        auto available = state.batch.row_count - state.offset;
        auto take = MinValue<idx_t>(available, STANDARD_VECTOR_SIZE - produced);
        for (idx_t column_index = 0; column_index < output.ColumnCount(); column_index++) {
            auto source_column =
                state.column_ids.empty() ? column_index : state.column_ids[column_index];
            auto& vector = output.data[column_index];
            auto& values = state.batch.columns[source_column];
            auto& valid = state.batch.valid[source_column];

            ValidityMask mask(STANDARD_VECTOR_SIZE);
            mask.SetAllValid(STANDARD_VECTOR_SIZE);
            for (idx_t i = 0; i < take; i++) {
                auto source_row = state.offset + i;
                auto target_row = produced + i;
                if (!valid[source_row]) {
                    mask.SetInvalid(target_row);
                    continue;
                }
                // 每行重新取指针：AddString 可能触发 vector buffer 扩容
                FlatVector::GetDataMutable<string_t>(vector)[target_row] =
                    StringVector::AddString(vector, values[source_row]);
            }
            FlatVector::SetValidity(vector, mask);
        }
        produced += take;
        state.offset += take;
    }

    output.SetCardinality(produced);
}

}  // namespace

void RegisterScanFunctions(ExtensionLoader& loader) {
    TableFunction hbase_scan("hbase_scan",
                             {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                             HbaseScanExecute, HbaseScanBind, HbaseScanInitGlobal);

    hbase_scan.GetSignature().WithTypedKwargs("options", [](TypedKwargs& options) {
        options.Add("start_row", LogicalType::VARCHAR)
            .Add("stop_row", LogicalType::VARCHAR)
            .Add("columns", LogicalType::LIST(LogicalType::VARCHAR))
            .Add("max_versions", LogicalType::INTEGER)
            .Add("timestamp_min", LogicalType::BIGINT)
            .Add("timestamp_max", LogicalType::BIGINT)
            .Add("cache", LogicalType::BOOLEAN);
    });

    hbase_scan.projection_pushdown = true;
    hbase_scan.parallelism = TableFunctionParallelism::FORCE_SINGLE_THREADED;
    loader.RegisterFunction(hbase_scan);
}

}  // namespace hbase
}  // namespace duckdb
