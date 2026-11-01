cmake_minimum_required(VERSION 3.5)

# 扩展名：构建产物为 hbase.duckdb_extension
set(TARGET_NAME hbase)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# gRPC / Protobuf 由 vcpkg 提供（见仓库根 vcpkg.json）
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# ---- 由 protocol/proto 生成 C++ 桩（不提交生成物） ----
set(PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/proto)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(GRPC_PROTO ${PROTO_ROOT}/hbase_data_service.proto)
set(GRPC_GEN_SRCS
    ${PROTO_GEN_DIR}/hbase_data_service.pb.cc
    ${PROTO_GEN_DIR}/hbase_data_service.grpc.pb.cc)
set(GRPC_GEN_HDRS
    ${PROTO_GEN_DIR}/hbase_data_service.pb.h
    ${PROTO_GEN_DIR}/hbase_data_service.grpc.pb.h)

add_custom_command(
  OUTPUT ${GRPC_GEN_SRCS} ${GRPC_GEN_HDRS}
  COMMAND protobuf::protoc
  ARGS --proto_path=${PROTO_ROOT}
       --cpp_out=${PROTO_GEN_DIR}
       --grpc_out=${PROTO_GEN_DIR}
       --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
       ${GRPC_PROTO}
  DEPENDS ${GRPC_PROTO}
  COMMENT "Generating protobuf/gRPC stubs for hbase_data_service.proto"
  VERBATIM)

include_directories(src/include)
include_directories(${PROTO_GEN_DIR})

set(EXTENSION_SOURCES
    src/hbase_extension.cpp
    src/common/connection_registry.cpp
    src/provider/grpc_provider.cpp
    src/provider/provider_factory.cpp
    src/sql/hbase_connect_function.cpp
    src/sql/hbase_scan_function.cpp
    ${GRPC_GEN_SRCS}
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

foreach(EXT_TARGET ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
  target_link_libraries(${EXT_TARGET} gRPC::grpc++ protobuf::libprotobuf)
endforeach()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
