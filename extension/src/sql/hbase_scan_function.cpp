#include "sql/hbase_scan_function.hpp"

#include "common/connection_registry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "provider/remote_scan_provider.hpp"

namespace duckdb {
namespace hbase {

namespace {

//! Bind 阶段解析出的扫描参数与已解析的列集合。
struct HbaseScanBindData : public TableFunctionData {
    ConnectionConfig connection;
    std::shared_ptr<IRemoteScanProvider> provider;
    string namespace_name;
    string table_name;
    vector<RemoteColumn> columns;
    Value start_row;
    Value stop_row;
    int32_t max_versions = 1;
    Value timestamp_min;
    Value timestamp_max;
    bool use_cache = true;
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

void ParseScanOptions(TableFunctionBindInput& input, HbaseScanBindData& result,
                      vector<string>& column_specs) {
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
    ConnectionEntry entry;
    if (!registry.TryGet(connection_name, entry)) {
        throw BinderException(
            "hbase_scan: 连接 '%s' 未注册，请先执行 SELECT hbase_connect('%s', 'grpc://host:port')",
            connection_name, connection_name);
    }

    result->connection = entry.config;
    result->provider = entry.provider;
    result->namespace_name = StringValue::Get(input.inputs[1]);
    result->table_name = StringValue::Get(input.inputs[2]);
    if (result->table_name.empty()) {
        throw BinderException("hbase_scan: 表名不能为空");
    }

    vector<string> column_specs;
    ParseScanOptions(input, *result, column_specs);

    if (!column_specs.empty()) {
        // 显式列：类型默认 VARCHAR（HBase 无类型信息，类型映射规则待定）
        for (auto& spec : column_specs) {
            result->columns.push_back(ParseColumnSpec(spec));
        }
    } else if (result->provider) {
        auto schema = result->provider->GetSchema(result->namespace_name, result->table_name);
        result->columns = schema.columns;
    }

    if (result->columns.empty()) {
        throw BinderException(
            "hbase_scan: 表 '%s.%s' 没有可用列，请显式指定 columns := ['cf:qualifier']",
            result->namespace_name, result->table_name);
    }

    for (auto& column : result->columns) {
        auto name =
            column.qualifier.empty() ? column.family : column.family + ":" + column.qualifier;
        names.push_back(Identifier(name));
        return_types.push_back(MapSqlType(column.sql_type));
    }

    return std::move(result);
}

void HbaseScanExecute(ClientContext& context, TableFunctionInput& data, DataChunk& output) {
    // Bind 已拿到 Schema；数据面（Arrow IPC 解码）在下一阶段实现。
    throw NotImplementedException("hbase_scan: Scan 数据面（Arrow IPC）尚未实现（M1 进行中）");
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
