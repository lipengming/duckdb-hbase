# gateway — Java HBase Gateway

本目录承载 Java 侧实现：gRPC 服务、Gateway SPI、HBase 访问适配器。构建系统为 Maven 多模块。

## 模块与依赖方向

```text
gateway-server  ──>  gateway-core  ──>  gateway-api
adapter-*       ──>  gateway-api
```

| 模块 | 职责 | 依赖 |
| --- | --- | --- |
| `gateway-api` | 协议 DTO、SPI 接口定义 | Protobuf |
| `gateway-core` | 服务实现、Factory、流式传输、安全 | `gateway-api` |
| `adapter-native` | HBase 原生客户端适配（默认实现） | `gateway-api`、HBase Client 2.x |
| `adapter-rest` | HBase REST 适配 | `gateway-api`、HTTP Client |
| `adapter-thrift` | HBase Thrift 适配 | `gateway-api`、Thrift Client |
| `adapter-phoenix` | Phoenix JDBC 适配 | `gateway-api`、Phoenix JDBC |
| `gateway-server` | gRPC 入口与装配 | `gateway-core`、gRPC、Arrow |

## 约束

- `gateway-server` 不得直接引用具体 Adapter，只通过 Factory 按配置装配。
- Adapter 只依赖 `gateway-api`，不得反向依赖 `gateway-core`。
- 资源用 `try-with-resources` 管理，禁止 `System.out`，日志用 SLF4J。

## 当前状态

仅目录占位，尚无代码。设计见 [`../docs/gateway-design.md`](../docs/gateway-design.md)，协议见 [`../docs/protocol.md`](../docs/protocol.md)。
