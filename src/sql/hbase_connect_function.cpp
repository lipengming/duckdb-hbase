#include "sql/hbase_connect_function.hpp"

#include "common/connection_registry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {
namespace hbase {

namespace {

//! hbase_connect(name, endpoint)：注册连接。重复注册同名连接时覆盖旧配置。
void HbaseConnectFunction(DataChunk& args, ExpressionState& state, Vector& result) {
    auto& context = state.GetContext();
    auto& registry = GetConnectionRegistry(context);

    for (idx_t i = 0; i < args.size(); i++) {
        auto name_value = args.GetValue(0, i);
        auto endpoint_value = args.GetValue(1, i);
        if (name_value.IsNull() || endpoint_value.IsNull()) {
            throw InvalidInputException("hbase_connect: 连接名与端点都不能为 NULL");
        }

        auto name = StringValue::Get(name_value);
        auto endpoint = StringValue::Get(endpoint_value);
        if (name.empty()) {
            throw InvalidInputException("hbase_connect: 连接名不能为空");
        }

        auto config = ParseEndpoint(name, endpoint);
        registry.Upsert(std::move(config));
        result.SetValue(i, Value::BOOLEAN(true));
    }
}

//! hbase_disconnect(name)：删除连接，返回是否真的删除了。
void HbaseDisconnectFunction(DataChunk& args, ExpressionState& state, Vector& result) {
    auto& context = state.GetContext();
    auto& registry = GetConnectionRegistry(context);

    for (idx_t i = 0; i < args.size(); i++) {
        auto name_value = args.GetValue(0, i);
        if (name_value.IsNull()) {
            throw InvalidInputException("hbase_disconnect: 连接名不能为 NULL");
        }
        result.SetValue(i, Value::BOOLEAN(registry.Remove(StringValue::Get(name_value))));
    }
}

//! hbase_version()：返回扩展版本，便于用户与日志定位。
void HbaseVersionFunction(DataChunk& args, ExpressionState& state, Vector& result) {
#ifdef EXT_VERSION_HBASE
    const char* version = EXT_VERSION_HBASE;
#else
    const char* version = "0.1.0-dev";
#endif
    result.SetVectorType(VectorType::CONSTANT_VECTOR);
    ConstantVector::SetNull(result, false);
    ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, version);
}

}  // namespace

void RegisterConnectFunctions(ExtensionLoader& loader) {
    // DuckDB 2.0 起，可能在执行期抛异常的函数必须显式标记 fallible（见 docs/coding-standards.md）。
    ScalarFunction connect_function("hbase_connect", {LogicalType::VARCHAR, LogicalType::VARCHAR},
                                    LogicalType::BOOLEAN, HbaseConnectFunction);
    connect_function.SetFallible();
    loader.RegisterFunction(connect_function);

    ScalarFunction disconnect_function("hbase_disconnect", {LogicalType::VARCHAR},
                                       LogicalType::BOOLEAN, HbaseDisconnectFunction);
    disconnect_function.SetFallible();
    loader.RegisterFunction(disconnect_function);

    loader.RegisterFunction(
        ScalarFunction("hbase_version", {}, LogicalType::VARCHAR, HbaseVersionFunction));
}

}  // namespace hbase
}  // namespace duckdb
