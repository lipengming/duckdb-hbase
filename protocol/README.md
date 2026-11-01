# protocol — 远程扫描协议

本目录定义 Extension 与 Gateway 之间的线协议：**Protobuf 承载控制面，Arrow IPC 承载数据面**。

## 文件

| 文件 | 说明 |
| --- | --- |
| `proto/hbase_data_service.proto` | 服务定义与消息结构草案（依据技术方案 §3.5） |

## 生成代码

生成产物不提交仓库，由构建流程生成：

```bash
# Java（M2 之后接入 Maven 构建）
protoc --proto_path=protocol/proto \
       --java_out=gateway/gateway-api/src/main/java \
       protocol/proto/hbase_data_service.proto

# C++（由 extension/CMakeLists.txt 调用，M2 之后）
protoc --proto_path=protocol/proto --cpp_out=<build-dir> protocol/proto/hbase_data_service.proto
```

## 兼容规则

1. 字段只增不删；删除需 `reserved` 编号与名称。
2. 字段编号发布后不复用，不修改既有字段类型与语义。
3. 每次变更同步更新 [`../docs/protocol.md`](../docs/protocol.md)，并在 PR 中说明兼容性影响。
4. 破坏性变更必须升版本包名（`duckdb.hbase.v2`），不得就地修改 v1。

## 当前状态

`hbase_data_service.proto` 为**草案**，尚未评审定稿；字段编号在协议评审通过前可变。
