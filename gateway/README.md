# gateway — Java HBase Gateway

本目录承载 Java 侧实现：gRPC 服务、Gateway SPI、HBase 访问适配器。构建系统为 Maven 多模块。

## 模块与依赖方向

```text
gateway-server  ──>  gateway-core  ──>  gateway-api
adapter-*       ──>  gateway-api
```

| 模块 | 职责 | 依赖 |
| --- | --- | --- |
| `gateway-api` | 协议生成代码（来自 `protocol/proto`）、SPI 接口与模型 | Protobuf、gRPC |
| `gateway-core` | 配置、`AdapterFactory`、`GatewayService` 编排 | `gateway-api` |
| `adapter-native` | HBase 原生客户端适配（默认实现） | `gateway-api`、HBase Client 2.x |
| `adapter-rest` | HBase REST 适配（M5） | `gateway-api`、HTTP Client |
| `adapter-thrift` | HBase Thrift 适配（M5） | `gateway-api`、Thrift Client |
| `adapter-phoenix` | Phoenix JDBC 适配（M5） | `gateway-api`、Phoenix JDBC |
| `gateway-server` | gRPC 入口与装配 | `gateway-core`、gRPC、Arrow |

## 构建与运行

```bash
# 构建（JDK 11 + Maven 3.9）
mvn -f gateway/pom.xml -DskipTests package

# 单元测试
mvn -f gateway/pom.xml test

# 启动（默认读取 conf/application.properties）
java -jar gateway/gateway-server/target/gateway-server-0.1.0-SNAPSHOT.jar gateway/conf/application.properties
```

## 当前状态（M1）

| 能力 | 状态 |
| --- | --- |
| Health / GetCapabilities / GetSchema / Cancel | 已实现，接通 Native Adapter |
| Scan / BatchGet | 返回 `UNIMPLEMENTED`：Arrow IPC 编码未完成 |
| 配置 | `.properties`（YAML 待 M2） |

## 约束

- `gateway-server` 不得直接引用具体 Adapter，只通过 `ServiceLoader<AdapterProvider>` 按 `gateway.adapter` 装配。
- Adapter 只依赖 `gateway-api`，不得反向依赖 `gateway-core`。
- 资源用 `try-with-resources` 管理，禁止 `System.out`，日志用 SLF4J。
- 包名注意：`native` 是 Java 关键字，Native Adapter 的包为 `com.duckdb.hbase.adapter.hbase`。

设计见 [`../docs/gateway-design.md`](../docs/gateway-design.md)，协议见 [`../docs/protocol.md`](../docs/protocol.md)。
