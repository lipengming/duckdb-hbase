#include "sql/hbase_scan_function.hpp"

#include "common/connection_registry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "provider/remote_scan_provider.hpp"
#include "scan/columnar_batch_decoder.hpp"
#include "scan/remote_scan_stream.hpp"

#include <algorithm>
#include <mutex>

namespace duckdb {
namespace hbase {

namespace {

//! Bind 阶段解析出的扫描参数与已解析的列集合。
struct HbaseScanBindData : public TableFunctionData {
    ConnectionConfig connection;
    std::shared_ptr<IRemoteScanProvider> provider;
    RemoteScanRequest request;
    idx_t max_partitions = 1;
};

//! 一个扫描分区（RowKey 区间）。
struct ScanPartition {
    string start_row;
    string stop_row;
    bool include_start = true;
    bool include_stop = false;
};

//! 执行期共享状态：分区队列 + 列映射。
struct HbaseScanGlobalState : public GlobalTableFunctionState {
    std::shared_ptr<IRemoteScanProvider> provider;
    RemoteScanRequest request;
    vector<ScanPartition> partitions;
    atomic<idx_t> next_partition{0};
    vector<column_t> column_ids;
    idx_t bind_column_count = 0;

    idx_t MaxThreads() const override { return partitions.empty() ? 1 : partitions.size(); }
};

//! 线程本地状态：每个线程持有自己的远端流。
struct HbaseScanLocalState : public LocalTableFunctionState {
    unique_ptr<RemoteScanStream> stream;
    DecodedBatch batch;
    idx_t offset = 0;
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
                      vector<string>& column_specs, bool& cache_explicit, bool& cache_value) {
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
            cache_explicit = true;
            cache_value = !value.IsNull() && BooleanValue::Get(value);
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

//! 求 (a, b) 之间的一个 RowKey；无法切分时返回空串。
string Midpoint(const string& a, const string& b) {
    idx_t index = 0;
    while (index < a.size() && index < b.size() && a[index] == b[index]) {
        index++;
    }
    if (index == a.size()) {
        // a 是 b 的前缀：a + (b[i] / 2)
        if (index < b.size() && static_cast<uint8_t>(b[index]) > 1) {
            string mid = a;
            mid.push_back(static_cast<char>(static_cast<uint8_t>(b[index]) / 2));
            return mid;
        }
        return string();
    }
    auto low = static_cast<uint8_t>(a[index]);
    auto high = static_cast<uint8_t>(b[index]);
    if (low + 1 >= high) {
        // 相邻字节，无法再切
        return string();
    }
    string mid = a.substr(0, index);
    mid.push_back(static_cast<char>((low + high) / 2));
    return mid;
}

void CollectBoundaries(const string& low, const string& high, idx_t parts,
                       vector<string>& boundaries) {
    if (parts <= 1) {
        return;
    }
    auto mid = Midpoint(low, high);
    if (mid.empty()) {
        return;
    }
    boundaries.push_back(mid);
    auto left = parts / 2;
    CollectBoundaries(low, mid, left, boundaries);
    CollectBoundaries(mid, high, parts - left, boundaries);
}

//! 把 [start, stop) 切成最多 parts 个分区；无法切分时退化为单分区。
vector<ScanPartition> BuildPartitions(const RemoteScanRequest& request, idx_t parts) {
    ScanPartition whole{request.start_row, request.stop_row, request.include_start,
                        request.include_stop};
    if (parts <= 1 || request.start_row.empty() || request.stop_row.empty()) {
        return {whole};
    }

    vector<string> boundaries;
    CollectBoundaries(request.start_row, request.stop_row, parts, boundaries);
    if (boundaries.empty()) {
        return {whole};
    }
    sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    vector<ScanPartition> result;
    auto previous = request.start_row;
    for (auto& boundary : boundaries) {
        if (boundary <= previous || boundary >= request.stop_row) {
            continue;
        }
        result.push_back(ScanPartition{previous, boundary, true, false});
        previous = boundary;
    }
    result.push_back(ScanPartition{previous, request.stop_row, true, request.include_stop});
    return result;
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

    if (!entry.capabilities.supports_scan) {
        throw BinderException(
            "hbase_scan: 当前 Gateway 未声明 supports_scan，无法执行扫描（adapter 能力不足）");
    }
    result->connection = entry.config;
    result->provider = entry.provider;
    result->max_partitions = entry.capabilities.max_parallel_scans > 0
                                 ? static_cast<idx_t>(entry.capabilities.max_parallel_scans)
                                 : 1;
    result->request.namespace_name = StringValue::Get(input.inputs[1]);
    result->request.table = StringValue::Get(input.inputs[2]);
    result->request.request_id = "scan-" + connection_name + "-" + result->request.table;
    if (result->request.table.empty()) {
        throw BinderException("hbase_scan: 表名不能为空");
    }

    bool cache_explicit = false;
    bool cache_value = false;
    vector<string> column_specs;
    ParseScanOptions(input, *result, column_specs, cache_explicit, cache_value);

    if (column_specs.empty()) {
        // 不做“退化为整列族”的静默降级：那会返回一堆 NULL，比报错更难排查
        throw BinderException("hbase_scan: 必须显式指定 columns := ['family:qualifier']。"
                              "HBase 没有固定 schema，当前版本不支持不带 columns 的扫描");
    }
    for (auto& spec : column_specs) {
        result->request.columns.push_back(ParseColumnSpec(spec));
    }

    if (cache_explicit && cache_value) {
        throw NotImplementedException(
            "hbase_scan: 缓存引擎尚未实现（M3）。请省略 cache 参数或显式 cache := false");
    }

    for (auto& column : result->request.columns) {
        auto name =
            column.qualifier.empty() ? column.family : column.family + ":" + column.qualifier;
        names.push_back(Identifier(name));
        return_types.push_back(MapSqlType(column.sql_type));
    }
    if (result->request.max_versions > 1) {
        // Gateway 会按 Cell 时间戳展开成多行，并追加时间戳列
        names.push_back(Identifier("__timestamp"));
        return_types.push_back(LogicalType::VARCHAR);
    }

    return std::move(result);
}

unique_ptr<GlobalTableFunctionState> HbaseScanInitGlobal(ClientContext& context,
                                                         TableFunctionInitInput& input) {
    auto& bind_data = input.bind_data->Cast<HbaseScanBindData>();
    auto state = make_uniq<HbaseScanGlobalState>();
    state->provider = bind_data.provider;
    state->request = bind_data.request;
    state->column_ids = input.column_ids;
    state->bind_column_count =
        bind_data.request.columns.size() + (bind_data.request.max_versions > 1 ? 1 : 0);
    state->partitions = BuildPartitions(bind_data.request, bind_data.max_partitions);
    return std::move(state);
}

unique_ptr<LocalTableFunctionState> HbaseScanInitLocal(ExecutionContext& context,
                                                       TableFunctionInitInput& input,
                                                       GlobalTableFunctionState* global_state) {
    return make_uniq<HbaseScanLocalState>();
}

void HbaseScanExecute(ClientContext& context, TableFunctionInput& data, DataChunk& output) {
    auto& state = data.global_state->Cast<HbaseScanGlobalState>();
    auto& local = data.local_state->Cast<HbaseScanLocalState>();
    output.Reset();
    idx_t produced = 0;

    while (produced < STANDARD_VECTOR_SIZE) {
        if (context.IsInterrupted()) {
            if (local.stream) {
                local.stream->Close();
            }
            throw InterruptException("hbase_scan: 查询被中断");
        }

        if (local.offset >= local.batch.row_count) {
            if (!local.stream) {
                auto index = state.next_partition.fetch_add(1);
                if (index >= state.partitions.size()) {
                    break;
                }
                auto& partition = state.partitions[index];
                auto request = state.request;
                request.start_row = partition.start_row;
                request.stop_row = partition.stop_row;
                request.include_start = partition.include_start;
                request.include_stop = partition.include_stop;
                request.request_id = state.request.request_id + "-p" + std::to_string(index);
                auto provider = state.provider;
                local.stream = make_uniq<RemoteScanStream>(
                    [provider, request](const RemoteBatchCallback& callback) {
                        provider->Scan(request, callback);
                    },
                    /*max_queued=*/2,
                    [provider, request]() { provider->Cancel(request.request_id); });
            }

            string payload;
            int64_t row_count = 0;
            if (!local.stream->Next(payload, row_count)) {
                local.stream.reset();
                local.batch = DecodedBatch();
                local.offset = 0;
                continue;
            }
            local.batch = DecodeColumnarBatch(payload);
            local.offset = 0;
            if (local.batch.row_count == 0) {
                continue;
            }
            if (local.batch.columns.size() != state.bind_column_count) {
                throw IOException("hbase_scan: 数据批列数(%llu)与绑定列数(%llu)不一致",
                                  static_cast<unsigned long long>(local.batch.columns.size()),
                                  static_cast<unsigned long long>(state.bind_column_count));
            }
        }

        auto available = local.batch.row_count - local.offset;
        auto take = MinValue<idx_t>(available, STANDARD_VECTOR_SIZE - produced);
        for (idx_t column_index = 0; column_index < output.ColumnCount(); column_index++) {
            auto source_column =
                state.column_ids.empty() ? column_index : state.column_ids[column_index];
            auto& vector = output.data[column_index];
            auto& values = local.batch.columns[source_column];
            auto& valid = local.batch.valid[source_column];

            ValidityMask mask(STANDARD_VECTOR_SIZE);
            mask.SetAllValid(STANDARD_VECTOR_SIZE);
            for (idx_t i = 0; i < take; i++) {
                auto source_row = local.offset + i;
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
        local.offset += take;
    }

    output.SetCardinality(produced);
}

}  // namespace

void RegisterScanFunctions(ExtensionLoader& loader) {
    TableFunction hbase_scan(
        "hbase_scan", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
        HbaseScanExecute, HbaseScanBind, HbaseScanInitGlobal, HbaseScanInitLocal);

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
    loader.RegisterFunction(hbase_scan);
}

}  // namespace hbase
}  // namespace duckdb
