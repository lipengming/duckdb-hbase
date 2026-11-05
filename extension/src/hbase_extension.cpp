#define DUCKDB_EXTENSION_MAIN

#include "hbase_extension.hpp"
#include "cache/cache_types.hpp"
#include "sql/hbase_cache_functions.hpp"
#include "sql/hbase_connect_function.hpp"
#include "sql/hbase_scan_function.hpp"

#include "duckdb.hpp"

namespace duckdb {

namespace hbase {

static void LoadInternal(ExtensionLoader& loader) {
    // 配置项必须在任何扫描/缓存函数之前注册，否则 SET hbase_cache_* 会报未知参数
    cache::RegisterCacheConfiguration(loader.GetDatabaseInstance());
    RegisterConnectFunctions(loader);
    RegisterScanFunctions(loader);
    RegisterCacheFunctions(loader);
}

}  // namespace hbase

void HbaseExtension::Load(ExtensionLoader& loader) {
    hbase::LoadInternal(loader);
}

std::string HbaseExtension::Name() {
    return "hbase";
}

std::string HbaseExtension::Version() const {
#ifdef EXT_VERSION_HBASE
    return EXT_VERSION_HBASE;
#else
    return "0.1.0-dev";
#endif
}

}  // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(hbase, loader) {
    duckdb::hbase::LoadInternal(loader);
}
}
