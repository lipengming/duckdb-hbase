#include "sql/hbase_scan_function.hpp"

#include "common/connection_registry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {
namespace hbase {

namespace {

//! Bind 阶段解析出的扫描参数（不包含远端 schema）。
struct HbaseScanBindData : public TableFunctionData {
    ConnectionConfig connection;
    string namespace_name;
    string table_name;
    vector<string> columns;
    Value start_row;
    Value stop_row;
    int32_t max_versions = 1;
    Value timestamp_min;
    Value timestamp_max;
    bool use_cache = true;
};

bool IsValidColumnSpec(const string& spec) {
    auto sep = spec.find(':');
    if (sep == string::npos || sep == 0) {
        return false;
    }
    // qualifier 允许为空（表示整列族），但 family 不能为空
    return spec.find(':', sep + 1) == string::npos;
}

void ParseScanOptions(TableFunctionBindInput& input, HbaseScanBindData& result) {
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
                result.columns.push_back(std::move(spec));
            }
        } else if (key == "start_row") {
            result.start_row = value;
        } else if (key == "stop_row") {
            result.stop_row = value;
        } else if (key == "max_versions") {
            if (!value.IsNull()) {
                result.max_versions = IntegerValue::Get(value);
                if (result.max_versions < 1) {
                    throw BinderException("hbase_scan: max_versions 必须 >= 1");
                }
            }
        } else if (key == "timestamp_min") {
            result.timestamp_min = value;
        } else if (key == "timestamp_max") {
            result.timestamp_max = value;
        } else if (key == "cache") {
            if (!value.IsNull()) {
                result.use_cache = BooleanValue::Get(value);
            }
        } else {
            throw BinderException("hbase_scan: 未知参数 '%s'", key.GetIdentifierName());
        }
    }

    if (!result.timestamp_min.IsNull() && !result.timestamp_max.IsNull()) {
        auto min_ts = BigIntValue::Get(result.timestamp_min);
        auto max_ts = BigIntValue::Get(result.timestamp_max);
        if (min_ts > max_ts) {
            throw BinderException("hbase_scan: timestamp_min(%lld) 不能大于 timestamp_max(%lld)",
                                  min_ts, max_ts);
        }
    }
}

unique_ptr<FunctionData> HbaseScanBind(ClientContext& context, TableFunctionBindInput& input,
                                       vector<LogicalType>& return_types,
                                       vector<Identifier>& names) {
    auto result = make_uniq<HbaseScanBindData>();

    auto connection_name = StringValue::Get(input.inputs[0]);
    auto& registry = GetConnectionRegistry(context);
    if (!registry.TryGet(connection_name, result->connection)) {
        throw BinderException(
            "hbase_scan: 连接 '%s' 未注册，请先执行 SELECT hbase_connect('%s', 'grpc://host:port')",
            connection_name, connection_name);
    }

    result->namespace_name = StringValue::Get(input.inputs[1]);
    result->table_name = StringValue::Get(input.inputs[2]);
    if (result->table_name.empty()) {
        throw BinderException("hbase_scan: 表名不能为空");
    }

    ParseScanOptions(input, *result);

    // M1 骨架：Schema 需要由 Gateway GetSchema 提供，RemoteScanProvider 尚未接入，
    // 因此这里无法确定输出列与类型。跟踪项见 docs/roadmap.md M1。
    throw NotImplementedException("hbase_scan: 已识别连接 '%s' (%s) 与表 '%s.%s'，但 "
                                  "RemoteScanProvider 尚未接入（M1 进行中）",
                                  connection_name, result->connection.endpoint,
                                  result->namespace_name, result->table_name);

    return std::move(result);
}

void HbaseScanExecute(ClientContext& context, TableFunctionInput& data, DataChunk& output) {
    // Bind 阶段会抛错，此函数在 Provider 接入后实现流式拉取。
    throw InternalException("hbase_scan: 执行阶段尚未实现（M1 进行中）");
}

}  // namespace

void RegisterScanFunctions(ExtensionLoader& loader) {
    TableFunction hbase_scan("hbase_scan",
                             {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                             HbaseScanExecute, HbaseScanBind);

    hbase_scan.GetSignature().WithTypedKwargs("options", [](TypedKwargs& options) {
        options.Add("start_row", LogicalType::VARCHAR)
            .Add("stop_row", LogicalType::VARCHAR)
            .Add("columns", LogicalType::LIST(LogicalType::VARCHAR))
            .Add("max_versions", LogicalType::INTEGER)
            .Add("timestamp_min", LogicalType::BIGINT)
            .Add("timestamp_max", LogicalType::BIGINT)
            .Add("cache", LogicalType::BOOLEAN);
    });

    loader.RegisterFunction(hbase_scan);
}

}  // namespace hbase
}  // namespace duckdb
