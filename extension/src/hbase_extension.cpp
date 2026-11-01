#define DUCKDB_EXTENSION_MAIN

#include "hbase_extension.hpp"
#include "sql/hbase_connect_function.hpp"
#include "sql/hbase_scan_function.hpp"

#include "duckdb.hpp"

namespace duckdb {

namespace hbase {

static void LoadInternal(ExtensionLoader& loader) {
    RegisterConnectFunctions(loader);
    RegisterScanFunctions(loader);
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
