# Gateway 设计

> 提炼自 [技术方案 V1.0](../tech_v0.1.md) §3。若与方案冲突，以方案为准。

本文描述 Gateway 抽象、Provider 能力协商、SPI 与 Adapter 装配方式。协议字段与错误模型见 [protocol.md](protocol.md)，分层与依赖方向见 [architecture.md](architecture.md)，Extension 侧设计见 [extension-design.md](extension-design.md)，缓存见 [cache-design.md](cache-design.md)，部署与安全见 [deployment-guide.md](deployment-guide.md)。第 8 节参考方案 §6.2，第 9、10 节参考 §8.2、§8.3。

## 1. 设计目标

HBase 有多种接入方式，能力与性能差异明显，不能把某一种客户端写死在 Extension 中。方案要求：

- Extension 无须了解底层接入协议，只依赖 `IRemoteScanProvider`。
- Gateway 无须了解 DuckDB 的内部执行模型，只负责远端数据访问。
- 切换接入方式时，SQL、Scan Planner、Cache 均不改变，只更换 Adapter 与配置。

SQL 解析与执行、Projection / Filter 计算、缓存命中判断与补列合并属于 Extension；Gateway 不承担这些职责，也不下沉 Cache 能力。

## 2. 两级抽象与职责划分

方案建议两级接口：Extension 侧 `IRemoteScanProvider`，Gateway 侧 `GatewayAdapter`（§3.6 给出具体接口名 `HBaseAccessAdapter`），两者通过统一 Gateway 协议通信。

| 维度 | Extension 侧 `IRemoteScanProvider` | Gateway 侧 `HBaseAccessAdapter` |
| --- | --- | --- |
| 所在进程 | DuckDB Extension（C++） | Java Gateway |
| 定位 | Extension 与 Gateway 之间唯一允许的依赖边界（§2.8） | Gateway 内部对 HBase 访问的二次抽象（§3.6） |
| 对上暴露 | 能力协商、Scan、BatchGet、Schema、Cancel、Health | 能力声明、Schema、ScanCursor、GetCursor、Cancel、Health |
| 对下依赖 | Gateway 协议（gRPC / Protobuf / Arrow IPC） | HBase Java Client、REST Client、Thrift Client、Phoenix JDBC |
| 返回形态 | `IScanStream` / `IGetStream`，逐批 `ArrowRecordBatch` | `ScanCursor` / `GetCursor`，不暴露 `ResultScanner` |
| 错误处理 | 将远端错误映射为 Extension 错误 | 将 HBase 客户端异常映射为协议错误 |
| 可替换性 | 连接串与配置驱动，不感知 Adapter 类型 | 由 Factory 按配置装配，Server 不直接引用实现 |

`ScanCursor` / `GetCursor` 是 Gateway 内部接口，不直接暴露 HBase 的 `ResultScanner`，避免 HBase 客户端对象泄漏到协议层。

## 3. Extension 侧 Provider 接口

| 方法 | 签名 | 说明 |
| --- | --- | --- |
| `GetCapabilities` | `ProviderCapabilities GetCapabilities()` | 获取当前 Gateway 能力集合，连接初始化时调用 |
| `Scan` | `unique_ptr<IScanStream> Scan(const ScanRequest &)` | 发起范围扫描，返回流式游标 |
| `BatchGet` | `unique_ptr<IGetStream> BatchGet(const BatchGetRequest &)` | 按 RowKey 批量读取，用于部分命中补列 |
| `GetSchema` | `TableSchema GetSchema(const TableIdentifier &)` | 获取 Family / Qualifier 与逻辑类型描述 |
| `Cancel` | `void Cancel(const string &request_id)` | 按请求 ID 取消远端任务 |
| `CheckHealth` | `ProviderHealth CheckHealth()` | 健康检查 |

`Scan()` 不直接返回 `vector<DataChunk>`：大数据量 Scan 必须流式处理，不能把全部 HBase 结果一次性加载到内存（§3.3）。流接口为：

| 接口 | 方法 | 说明 |
| --- | --- | --- |
| `IScanStream` | `bool Next(ArrowRecordBatch &batch)` / `void Cancel()` | 取下一批数据；无更多数据返回 false；取消并释放远端资源 |
| `IGetStream` | `bool Next(ArrowRecordBatch &batch)` / `void Cancel()` | 取下一批批量读取结果；取消并释放远端资源 |

Provider 会话职责（§2.8）：Provider Session 创建、能力协商、Scan、Batch Get、Schema Metadata、取消任务、远端错误映射。Cache Manager 可一边接收远端数据、一边写本地 Parquet，不需要完整数据集驻留内存。

## 4. ProviderCapabilities 能力矩阵

不同接入方式能力不同，Extension 不能假定所有 Gateway 支持同样的功能，必须在连接初始化时协商（§3.4）。

| 能力字段 | 类型 | 语义 | 不满足时的处理 |
| --- | --- | --- | --- |
| `supports_scan` | bool | 是否支持范围 Scan | 为 false 时该 Provider 不可用于 Scan |
| `supports_batch_get` | bool | 是否支持 Batch Get | 为 false 时 Partial Hit 退化为范围 Scan |
| `supports_rowkey_range` | bool | 是否支持 RowKey 范围 | 为 false 时不能按范围裁剪远端请求 |
| `supports_column_projection` | bool | 是否支持列裁剪 | 为 false 时只能取回后在本地裁剪 |
| `supports_timestamp` | bool | 是否支持时间戳语义 | 为 false 时不能当作支持特定 HBase 时间戳语义的 Provider |
| `supports_versions` | bool | 是否支持多版本 | 为 false 时不能承诺历史版本语义 |
| `supports_filter_pushdown` | bool | 是否支持过滤下推 | 为 false 时 Filter 仅在 Extension 本地执行 |
| `supports_async_scan` | bool | 是否支持异步 Scan | 为 false 时降级为同步流式读取 |
| `supports_cancellation` | bool | 是否支持取消 | 为 false 时查询取消只能放弃本地读取 |
| `max_batch_size` | int | 单批最大行数 | 请求的 `batch_size` 不得超过该值 |
| `max_parallel_scans` | int | 最大并行 Scan 数 | 并行度不得超过该值 |

方案对各接入方式只给出定性结论：Native Java Client 支持原生 Scan 和 Batch Get；REST 是否支持批量 Get 需按具体 REST Server 版本确认；Phoenix JDBC 不一定能返回原生 HBase Cell Timestamp；部分自定义 Gateway 可能只支持 Scan、不支持 Get。逐项布尔能力值待定（TBD），须由集成测试或运行时探测确认。

## 5. Gateway SPI 与 Adapter

Gateway SPI（`HBaseAccessAdapter`）是 Gateway 内部契约，Adapter 是具体实现；Factory 按配置选择实现，Gateway Server 只依赖 SPI。SPI 方法：`capabilities()`、`getSchema()`、`scan()`、`batchGet()`、`cancel()`、`health()`。

| Adapter | 底层依赖 | 方案定位 | 建议优先级 | 关键限制 |
| --- | --- | --- | --- | --- |
| `NativeHBaseAdapter` | HBase Connection / Table / Scan / Get / ResultScanner | 默认方案 | P0，M1 首先交付 | 需要 Java Gateway 与 HBase 客户端依赖 |
| `RestHBaseAdapter` | RestClient | 兼容方案 | P1，按环境启用 | 数据传输效率较低；Batch Get 依赖 REST Server 版本 |
| `ThriftHBaseAdapter` | HBaseThriftClient | 兼容方案 | P2，按环境启用 | 能力和版本兼容需验证 |
| `PhoenixAdapter` | DataSource / Phoenix JDBC | 特殊适配 | P3，仅特定场景 | SQL 查询语义与原生 Cell Scan 语义不同，须限制支持的 Scan 语义 |
| Custom Adapter | 自定义 RPC / HTTP | 扩展方案 | 按需 | 需要先实现统一协议 |

优先级说明：方案只给出“默认 / 兼容 / 特殊 / 扩展”的定位，未给出数值优先级；P0-P3 为工程落地建议。方案明确第一阶段优先完成 Native Java Adapter，不提前实现所有 Adapter。

Phoenix 不是 HBase 原生 Scan 的等价替代：它提供 SQL 层访问能力，可能具有独立的事务、索引、视图及查询语义。Phoenix Adapter 必须限制支持的 Scan 语义，并在能力声明中明确不支持的部分，不能假定其完全支持原生 HBase 的 Cell、时间戳和版本语义。建议为 Adapter SPI 增加版本号，避免不同 Gateway 实现与 Extension 协议之间产生不兼容（§3.7），版本号形式待定（TBD）。

## 6. Gateway Factory 与配置化装配

Gateway 不应该通过修改代码选择，而应该通过配置选择（§3.7）。配置示例：

```yaml
gateway:
  adapter: native-java
  server:
    host: 127.0.0.1
    port: 18080
  hbase:
    quorum: zk1:2181,zk2:2181,zk3:2181
    znode_parent: /hbase
    rpc_timeout_ms: 30000
  pool:
    max_connections: 20
  security:
    mode: kerberos
    principal: hbase/_HOST@EXAMPLE.COM
    keytab: /etc/security/keytabs/hbase.keytab
```

装配流程：

1. Gateway 启动时读取 `application.yml`（发行包通过 `--spring.config.location` 指定）。
2. 解析 `GatewayConfig`，校验 `adapter` 取值（`native-java` / `rest` / `thrift` / `phoenix`）。
3. `AdapterFactory.create(config)` 按枚举分支构造对应 Adapter；未知取值抛 `UnsupportedOperationException`。
4. 注入 `hbase.quorum`、`hbase.znode_parent`、`hbase.rpc_timeout_ms`、`pool.max_connections` 与 `security` 参数，建立 HBase 连接。
5. 启动 gRPC Server，绑定 `server.host:server.port`，注册统一协议服务。
6. Server 仅持有 `HBaseAccessAdapter` 引用，不直接引用 `NativeHBaseAdapter` 等实现。
7. 暴露健康检查入口，供部署与运维探活。

TLS 证书、Gateway API 认证方式等安全参数方案未给出配置键名，待定（TBD）。

## 7. 替换 Gateway 时的依赖变化

以 Native Java 切换到 REST 为例（§3.8）：

| 项目 | 是否变化 | 说明 |
| --- | --- | --- |
| DuckDB Extension | 不变 | 只依赖 `IRemoteScanProvider` |
| Cache Engine | 不变 | 缓存属于 Extension，与接入方式无关 |
| Scan Planner | 不变 | Projection / Filter / Range 规划不变 |
| SQL | 不变 | `hbase_scan(...)` 等用户写法不变 |
| Gateway 协议 | 不变 | Protobuf + Arrow IPC 保持不变 |
| Gateway Adapter | 变化 | `NativeHBaseAdapter` 换成 `RestHBaseAdapter` |
| Gateway 配置 | 变化 | `gateway.adapter` 由 `native-java` 改为 `rest` |
| Gateway 依赖 JAR | 变化 | HBase Client 2.x 换成 HTTP Client |
| 能力声明 | 可能变化 | REST 的 Batch Get 等能力需按版本确认，可能触发降级 |

能力差异可能导致执行计划退化（例如 Batch Get 退化为范围 Scan），但 Extension 代码与用户 SQL 不变。这正是两级抽象希望实现的核心解耦效果。

## 8. Java Maven 模块划分

| 模块 | 依赖 | 职责 |
| --- | --- | --- |
| `gateway-api` | Protobuf、统一 DTO | 协议生成物、请求 / 响应 DTO、公共类型 |
| `gateway-core` | `gateway-api`、SPI | SPI 定义、能力模型、Factory、配置解析 |
| `adapter-native` | `gateway-api`、HBase Client 2.x | Native Java Scan / Batch Get 适配 |
| `adapter-rest` | `gateway-api`、HTTP Client | HBase REST 适配 |
| `adapter-thrift` | `gateway-api`、Thrift Client | HBase Thrift 适配 |
| `adapter-phoenix` | `gateway-api`、Phoenix JDBC | Phoenix SQL 查询适配 |
| `gateway-server` | `gateway-core`、gRPC、Arrow | gRPC 服务、流式传输、请求路由、生命周期 |

依赖方向：`gateway-server` → `gateway-core` → `gateway-api`；`adapter-native` / `adapter-rest` / `adapter-thrift` / `adapter-phoenix` 均只依赖 `gateway-api`。`adapter-*` 不得反向依赖 `gateway-core` 或 `gateway-server`；`gateway-server` 不直接引用具体 Adapter，由 Factory 装配 `HBaseAccessAdapter`。

## 9. 安全

方案要求必须支持（§8.2）：Kerberos 认证、TLS、Gateway API 认证、HBase Namespace / Table 权限控制、SQL 参数与配置的安全处理、审计日志。

| 安全项 | Gateway 侧职责 |
| --- | --- |
| Kerberos | 使用 `security.principal` / `keytab` 完成登录并维护 HBase 连接凭据；续期策略待定（TBD） |
| TLS | 保护 Extension 到 Gateway、Gateway 到 HBase 的传输通道；证书与开关配置待定（TBD） |
| Gateway API 认证 | 校验调用方身份，拒绝未认证请求；认证方式待定（TBD） |
| 权限控制 | 按 Namespace / Table 校验访问权限，越权请求返回授权错误 |
| 输入安全 | 不接受客户端随意传入 Java 类名、SQL 拼接片段或任意 HBase Filter 序列化对象 |
| Phoenix SQL | 必须采用参数绑定，禁止字符串拼接 SQL |
| 审计日志 | 记录请求 ID、调用方、表、操作类型与结果；日志字段待定（TBD） |
| 部署边界 | Gateway 必须部署在能够访问 HBase 集群的网络环境中 |

## 10. 超时与重试

HBase RPC 超时由 `hbase.rpc_timeout_ms` 配置（示例值 30000）。重试策略以方案 §8.3 为准：

| 操作 | 方案策略 | Gateway 侧职责 |
| --- | --- | --- |
| Schema 查询 | 可有限重试 | 对 HBase 元数据查询做有限重试并返回明确错误 |
| Scan 建立 | 建立失败可以重试 | 仅在尚未返回数据时重试，避免重复结果 |
| Scan 流读取 | 需谨慎，避免重复返回数据 | 中断后不得静默从头重扫 |
| Batch Get | 按 RowKey 批次重试 | 以 RowKey 批次为粒度重试，只返回确认成功的数据 |
| Cache 写入 | 允许 Abort 后重试 | 由 Extension 负责，Gateway 不参与 |
| Cancel | 幂等 | 重复 Cancel 同一 `request_id` 结果一致 |

不能对一个已经返回部分结果的 Scan 直接从头重试，再把新数据拼接到旧数据中。若未来支持断点续传，需要引入最后处理的 RowKey 和稳定的扫描语义，具体协议字段待定（TBD）。

查询取消时必须同时关闭远端 Scanner 和本地文件 Reader；远端 Scan 请求应携带 Request ID，便于取消和追踪（§8.1）。

## 11. 待定事项（TBD）

- 各 Adapter 的逐项能力布尔值：方案仅给出定性结论，需按 HBase / REST Server 版本与集成测试确认。
- `CapabilityRequest`、`CapabilityResponse`、`SchemaRequest`、`SchemaResponse`、`CancelRequest`、`CancelResponse`、`HealthRequest`、`HealthResponse` 的字段与编号，方案 §3.5 未定义。
- 协议错误码枚举与 `ErrorResponse` 字段，方案仅要求“错误码和错误详情”。
- TLS 证书、Gateway API 认证方式、审计日志字段的配置键。
- Adapter SPI 版本号的形式与兼容窗口；断点续传所需的最后 RowKey 与稳定扫描语义。

## 12. M1 实现状态

已落地的部分（`gateway/`）：

| 模块 | 内容 |
| --- | --- |
| `gateway-api` | `HBaseAccessAdapter` SPI、`AdapterProvider`（ServiceLoader 发现）、`AdapterCapabilities`、`TableSchema`、`Row`/`ScanCursor`、`HBaseAccessException`；由 `protocol/proto` 生成 gRPC 桩 |
| `gateway-core` | `GatewayConfig`（`.properties`）、`AdapterFactory`（按 `gateway.adapter` 装配）、`GatewayService` 编排 |
| `adapter-native` | `NativeHBaseAdapter`：Scan / BatchGet / 表结构 / 取消 / 健康检查，基于 HBase Java Client 2.6 |
| `gateway-server` | gRPC 服务：Health / GetCapabilities / GetSchema / Cancel / **Scan / BatchGet** 全部接通；Scan/BatchGet 按批编码流式返回；失败时附带 `ErrorInfo` trailing metadata |

两点实现约定：

1. **包名偏差**：Java 关键字 `native` 不能作为包名片段，因此 Native Adapter 的包为 `com.duckdb.hbase.adapter.hbase`，与 §6.2 中的 `adapter.native` 命名约定不同，模块名 `adapter-native` 保持不变。
2. **装配方式**：`AdapterFactory` 通过 `ServiceLoader<AdapterProvider>` 发现实现（`META-INF/services`），`gateway-server` 对 `adapter-native` 只声明 runtime 依赖，编译期不引用具体 Adapter，满足“server 不直接引用 Adapter”的约束。

## 13. 相关文档

- [架构说明](architecture.md)、[Extension 设计](extension-design.md)
- [缓存设计](cache-design.md)、[协议设计](protocol.md)、[Protobuf 定义](../protocol/proto/hbase_data_service.proto)
- [部署指南](deployment-guide.md)
