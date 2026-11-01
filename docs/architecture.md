# DuckDB-HBase 架构说明

> 提炼自 [技术方案 V1.0](../tech_v0.1.md) §1、§2、§6。若与方案冲突，以方案为准。

## 1. 项目定位

DuckDB-HBase 是面向 HBase 2.x 的 DuckDB 外部数据源扩展，提供 SQL 查询、列式 Scan Cache 与多接入方式适配能力。按方案的最终定位，它不只是一个 SQL Connector，而是具备多接入适配能力和本地列式 Scan Cache 的 HBase 分析型数据访问引擎。

技术约束：HBase 2.x、自建 Hadoop/HBase 集群、独立 Java Gateway、C++ DuckDB Extension、初期不实现 DuckDB Catalog、第一阶段只读、Arrow IPC 流式数据交换。

## 2. 架构分层

方案将系统划分为四层（Extension、Remote Provider Protocol、Gateway Adapter、HBase Infrastructure）。工程实现上可细分为下表七层：前三层同属 C++ Extension 进程，协议层由 Extension 客户端与 Gateway 服务端共同实现，Gateway 层与数据源层位于 Extension 之外。

| 层 | 主要组成 | 职责 | 归属 |
| --- | --- | --- | --- |
| SQL 层 | Table Function、Scalar Function、SET 配置项 | 把 SQL 转换为统一 ScanRequest，暴露连接与缓存管理入口 | Extension `sql` |
| 执行层 | ScanPlanner、ScanExecutor、ScanPipeline、DataChunkConverter | 规划 Projection / Filter / RowKey Range，串联 Cache 与 Provider，填充 DataChunk | Extension `scan` |
| Cache 层 | CacheManager、LookupEngine、CoverageIndex、SegmentManager、Merger、Invalidation | 覆盖度判断、读写、按 RowKey 补列与合并、失效与淘汰 | Extension `cache` + `storage` |
| 协议层 | Protobuf 定义、gRPC Streaming、Arrow IPC | 定义 Scan / BatchGet / GetSchema / Capabilities / Cancel / Health 的统一远程语义 | `protocol/proto` |
| Gateway 层 | gateway-server、gateway-core、gateway-api、adapter-* | 协议转换、请求路由、连接管理、HBase 访问适配 | Java Gateway |
| 数据源层 | HBase 2.x（自建集群） | 实际存储与 RegionServer 级 Scan/Get 语义 | 外部系统 |
| 构建层 | C++ CMake、Java Maven、scripts | 各自独立构建，Monorepo 统一存放 | Monorepo 根 |

数据源层是外部系统；Extension 各层、协议层与 Gateway 层均为自研组件。构建层不参与运行时调用链。

## 3. 依赖方向与边界

调用方向单向收敛：SQL 只依赖执行层，执行层只依赖 Cache 与 Provider 抽象，Provider 只依赖协议，协议由 Gateway 消费，Gateway 通过 SPI 选择 Adapter。

```
SQL Table/Scalar Function
            |
            v
Scan Planner / Executor / Pipeline
            |
     +------+-------------------+
     v                          v
ICacheManager            IRemoteScanProvider
(Cache Engine, 本地)      (唯一远端依赖边界)
     |                          |
     v                          v
本地 Segment / Parquet   Remote Provider Protocol
                          (gRPC + Protobuf + Arrow IPC)
                                |
                                v
                          Gateway Server
                                |
                        GatewayAdapter SPI
              +---------+-------+-------+---------+
              v         v       v       v         v
            Java      REST   Thrift  Phoenix   Custom
              +---------+-------+-------+---------+
                                |
                                v
                            HBase 2.x
```

必须守住的边界：

- Extension 不得直接依赖任何具体 Gateway 实现。执行器成员只能是 `shared_ptr<IRemoteScanProvider>`，不允许出现 `GrpcClient`、`RestClient` 之类的具体客户端。
- `scan` 不得 `include` `grpc_provider.hpp`，只能依赖 `remote_scan_provider.hpp`；接口头文件与实现分目录存放。
- Cache Engine 属于 Extension 核心能力，独立于 HBase 与 Gateway，切换接入方式时 Cache、Scan Planner、SQL 均不变。
- Gateway Server 不直接引用 `NativeHBaseAdapter`，由 Factory 依据配置装配 `HBaseAccessAdapter`。
- `catalog` 模块不是 DuckDB Catalog，只是轻量的 HBase Schema Metadata Provider。
- 能力差异由 `ProviderCapabilities` 协商表达，不能在 Extension 中假定所有 Gateway 支持同样的语义。

## 4. 模块边界

| 模块 | 职责 | 对外接口 / 入口 | 核心类 |
| --- | --- | --- | --- |
| extension | 扩展加载、函数注册、配置注册、生命周期 | `HBaseExtension::Load()` | `HBaseExtension`、`HBaseExtensionConfig`、`HBaseExtensionState`、`HBaseFunctionRegistry` |
| sql | Table Function / Scalar Function 与参数解析 | `hbase_scan`、`hbase_get`、`hbase_connect` 等 | `HBaseScanFunction`、`HBaseGetFunction` |
| scan | Scan 规划、执行与类型转换 | `ScanPlan`、`ScanRequest`、`DataChunk` | `ScanPlanner`、`ScanExecutor`、`ScanPipeline`、`DataChunkConverter` |
| cache | 覆盖判断、读写、合并、失效 | `ICacheManager`、`ICacheReader`、`ICacheWriter` | `CacheManager`、`CacheLookupEngine`、`CacheMerger` |
| storage | Parquet / 内存数据与 Metadata 持久化 | `ICacheStorage` | `ParquetCacheStorage`、`MemoryCacheStorage`、`MetadataStore` |
| provider | Gateway Client、协议适配、能力协商 | `IRemoteScanProvider`、`IScanStream`、`IGetStream` | `GrpcProvider`、`HttpProvider`、`GatewayProviderFactory` |
| catalog | Family/Qualifier 与逻辑类型映射 | Schema 查询结果 | `SchemaProvider`、`ColumnMapping`、`TypeMapping` |
| common | 配置、异常、日志、类型、RowKey | 公共头文件 | `Config`、`Errors`、`RowKey` |

依赖约束：`sql` 依赖 `scan`；`scan` 依赖 `cache`、`provider`、`catalog`、`common`；`cache` 依赖 `storage`；`provider` 依赖 `common` 与协议类型。反向依赖一律不允许。

## 5. 端到端查询链路

```
SQL
 -> Table Function Bind（参数解析、Schema 获取、输出列构建）
 -> Scan Planner（Required Columns、Filter、RowKey Range、ScanConsistency）
 -> Cache Lookup（按 Segment 返回 available_columns / missing_columns / uncached_ranges）
    -> Full Hit：Cache Scan 读取已缓存列
    -> Partial Hit：Cache Scan + 缺失列 BatchGet/Scan + RowKey Merge
    -> Miss：Gateway Scan，边接收边写 Cache
 -> Provider -> Gateway -> HBase
 -> Arrow RecordBatch Stream（流式，不整体驻留内存）
 -> DataChunkConverter -> DataChunk
 -> DuckDB 后续算子
```

各步要点：

1. `Bind` 阶段解析位置参数与命名参数（`start_row`、`stop_row`、`columns`、`max_versions`、`timestamp_min`、`timestamp_max`、`cache`），向 `catalog` 取表结构并生成输出列。
2. Projection 与 Filter 必须分开：Required Columns = Output Columns ∪ Filter Columns，只读 SELECT 列会导致 WHERE 无法正确执行。
3. Cache 状态不是简单的三分类，Lookup 返回逐 Segment 的覆盖计划，不同 RowKey 区间可以有不同缺失列。
4. 命中判定需同时满足：RowKey 范围完整覆盖、Required Columns 全部覆盖、时间戳语义兼容、未过期、数据完整。
5. `LIMIT` 不得影响缓存完整性，不能把 LIMIT 后的结果当作完整 RowKey 范围的缓存。
6. RowKey 一律按二进制处理，缓存覆盖比较使用与 HBase 相同的字节序规则。

## 6. 关键架构决策与非目标

| 决策 | 内容 | 理由与影响 |
| --- | --- | --- |
| D1 不实现 DuckDB Catalog | 仅提供 Schema Metadata Provider，通过 Table Function 显式传参访问表 | 避免绑定 DuckDB Catalog 注册机制；代价是查询必须写全连接名、namespace、表名 |
| D2 第一阶段只读 | 只提供 Scan / Get / 元数据 / 缓存管理，不提供写入 | 先把 Scan 闭环、缓存正确性与协议稳定性做扎实 |
| D3 不提前实现所有 Adapter | 优先完成 Native Java Adapter，REST/Thrift/Phoenix 后续按 SPI 补齐 | 多 Adapter 能力不对等，需以 Capabilities 协商为前提逐个验证 |

明确非目标：

- 不实现 DuckDB 原生 Catalog 注册，也不做 HBase 表的 DDL。
- 第一阶段不支持写入、更新、删除与事务语义。
- 不承诺 Phoenix Adapter 与原生 HBase Cell / 时间戳 / 版本语义等价。
- 不在设计阶段预设性能加速倍数，需以真实数据测量。
- 不在 Extension 内实现 HBase 协议，Gateway 也不承担 SQL 执行。

## 7. 文档导航

| 文档 | 内容 |
| --- | --- |
| [extension-design.md](extension-design.md) | C++ 模块、命名空间、SQL 函数、Table Function 各阶段、Scan 执行与类型转换 |
| [gateway-design.md](gateway-design.md) | Java Gateway 分包、Adapter SPI、Factory 装配与部署形态 |
| [protocol.md](protocol.md) | Protobuf 消息、gRPC 服务、Arrow IPC 载荷与错误模型 |
| [cache-design.md](cache-design.md) | Segment 模型、覆盖索引、补列合并、失效与淘汰 |
| [user-guide.md](user-guide.md) | 安装、连接配置、SQL/Python 用法与缓存管理 |
| [deployment-guide.md](deployment-guide.md) | Gateway 部署、安全、超时与运维参数 |
| [coding-standards.md](coding-standards.md) | 命名、目录、错误处理与测试约定 |
| [roadmap.md](roadmap.md) | M1-M5 里程碑与交付标准 |

> 详见 [技术方案 §1](../tech_v0.1.md)、[§2.1](../tech_v0.1.md)、[§3.2](../tech_v0.1.md)、[§6](../tech_v0.1.md)、[§11](../tech_v0.1.md)。
