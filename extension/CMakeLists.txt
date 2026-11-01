cmake_minimum_required(VERSION 3.5)

# 扩展名：构建产物为 hbase.duckdb_extension
set(TARGET_NAME hbase)

# 依赖通过 vcpkg 提供（见 vcpkg.json）。当前骨架阶段只使用 DuckDB 自带能力；
# gRPC / Protobuf 在接入 RemoteScanProvider 时再加入 vcpkg.json 与 find_package。

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

set(EXTENSION_SOURCES
    src/hbase_extension.cpp
    src/common/connection_registry.cpp
    src/sql/hbase_connect_function.cpp
    src/sql/hbase_scan_function.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
