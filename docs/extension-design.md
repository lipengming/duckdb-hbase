# DuckDB-HBase Extension 设计

> 提炼自 [技术方案 V1.0](../tech_v0.1.md) §2、§4、§6、§7。若与方案冲突，以方案为准。

## 1. 命名空间与模块划分

Extension 用 C++ 实现，按领域拆分模块，命名空间统一收敛在 `duckdb::hbase` 下：

```cpp
namespace duckdb { namespace hbase {
namespace sql {} namespace scan {} namespace cache {} namespace storage {}
namespace provider {} namespace catalog {} namespace common {}
}}
```

接口头文件与实现分目录：`include/provider/` 放 `remote_scan_provider.hpp`、`provider_capabilities.hpp`、`provider_session.hpp`，`src/provider/` 放 `grpc_provider.cpp`、`http_provider.cpp`、`provider_factory.cpp`。`scan` 不得 include `grpc_provider.hpp`，只能依赖 `remote_scan_provider.hpp`。

| 模块 | 主要源文件 | 核心职责 | 对外接口 |
| --- | --- | --- | --- |
| extension | `hbase_extension.cpp`、`hbase_extension_state.cpp`、`function_registry.cpp` | 加载、函数与配置注册、生命周期 | `HBaseExtension::Load()` |
| sql | `hbase_scan_function.cpp`、`hbase_get_function.cpp`、`hbase_connect_function.cpp`、`hbase_cache_functions.cpp`、`hbase_metadata_functions.cpp` | 参数解析、Schema 获取、输出列构建 | `hbase_scan`、`hbase_get`、`hbase_connect` 等 |
| scan | `scan_planner.cpp`、`scan_executor.cpp`、`scan_pipeline.cpp`、`projection_planner.cpp`、`predicate_planner.cpp`、`rowkey_range_planner.cpp`、`data_chunk_converter.cpp` | 规划与执行、Arrow 转 DataChunk | `ScanPlan`、`ScanRequest` |
| cache | `cache_manager.cpp`、`cache_lookup_engine.cpp`、`cache_coverage_index.cpp`、`cache_segment_manager.cpp`、`cache_merger.cpp`、`cache_invalidation.cpp`、`cache_eviction.cpp` | 覆盖判断、读写、合并、失效、淘汰 | `ICacheManager`、`ICacheReader`、`ICacheWriter` |
| storage | `parquet_cache_storage.cpp`、`memory_cache_storage.cpp`、`metadata_store.cpp`、`segment_manifest.cpp` | Parquet/内存数据与 Metadata 持久化 | `ICacheStorage` |
| provider | `grpc_provider.cpp`、`http_provider.cpp`、`provider_factory.cpp`、`arrow_stream_reader.cpp` | 会话、能力协商、Scan/Get、取消、错误映射 | `IRemoteScanProvider`、`IScanStream`、`IGetStream` |
| catalog | `schema_provider.cpp`、`column_mapping.cpp`、`type_mapping.cpp` | Family/Qualifier 与逻辑类型映射 | Schema 查询结果 |
| common | `config.cpp`、`errors.cpp`、`logger.cpp`、`types.cpp`、`rowkey.cpp` | 配置、异常、日志、类型、RowKey | 公共头文件 |

`catalog` 不是 DuckDB Catalog：不注册原生 Catalog，只提供轻量 Schema Metadata Provider。

> 详见 [技术方案 §2.1](../tech_v0.1.md)、[§6.1](../tech_v0.1.md)。

## 2. SQL 层函数清单

`HBaseExtension::Load()` 依次调用 `RegisterHBaseScanFunction()`、`RegisterHBaseGetFunction()`、`RegisterConnectionFunctions()`、`RegisterCacheFunctions()`、`RegisterConfiguration()`。

| API | 类型 | 作用 | 优先级 |
| --- | --- | --- | --- |
| `hbase_connect()` | Scalar Function | 注册连接（名称 + endpoint 或 JSON 配置） | 待定（TBD） |
| `hbase_disconnect()` | Scalar Function | 关闭连接 | 待定（TBD） |
| `hbase_scan()` | Table Function | 范围 Scan | P0 |
| `hbase_get()` | Table Function | 按 RowKey 查询 | P1 |
| `hbase_tables()` | Table Function | 查询表列表 | P1 |
| `hbase_columns()` | Table Function | 查询列信息 | P1 |
| `hbase_cache_stats()` | Table Function | 缓存统计 | P0 |
| `hbase_cache_clear()` | Table Function | 清理缓存（可带连接/namespace/表，也可无参清全部） | P0 |
| `hbase_connections()` | Table Function | 查看连接 | P1 |

`hbase_scan` 统一签名：

```
hbase_scan(
    connection_name, namespace, table_name,
    start_row := NULL, stop_row := NULL, columns := NULL,
    max_versions := 1,
    timestamp_min := NULL, timestamp_max := NULL,
    cache := true
)
```

`start_row` / `stop_row` 第一版为左闭右开；二进制 RowKey 另提供 BLOB 输入形式，不能一律转 UTF-8。`cache` 控制单次查询是否使用缓存，全局开关为 `SET hbase_cache_enabled`；相关配置项还有 `hbase_cache_ttl`、`hbase_cache_max_size`、`hbase_cache_directory`、`hbase_cache_mode`（如 `parquet`）。连接 JSON 中可出现 `endpoint`、`timeout_ms`、`cache_enabled`、`cache_ttl`。

> 详见 [技术方案 §2.2](../tech_v0.1.md)、[§2.3](../tech_v0.1.md)、[§4.1](../tech_v0.1.md)、[§4.2](../tech_v0.1.md)。

## 3. hbase_scan() Table Function 设计

接口职责示意（具体签名按项目锁定的 DuckDB 版本校准）：

```cpp
class HBaseScanFunction {
public:
    static unique_ptr<FunctionData> Bind(
        ClientContext &context, TableFunctionBindInput &input,
        vector<LogicalType> &return_types, vector<string> &names);

    static unique_ptr<GlobalTableFunctionState> InitGlobalState(
        ClientContext &context, TableFunctionInitInput &input);

    static unique_ptr<LocalTableFunctionState> InitLocalState(
        ExecutionContext &context, TableFunctionInitInput &input,
        GlobalTableFunctionState *global_state);

    static OperatorResultType Execute(
        ExecutionContext &context, TableFunctionInput &data, DataChunk &output);
};
```

| 阶段 | 职责 | 失败处理 |
| --- | --- | --- |
| Bind | 解析位置/命名参数，取 Schema，构建输出列（`return_types` / `names`） | 参数非法或 Schema 不可得时抛错，不进入执行 |
| InitGlobalState | 构建全局状态：ScanPlan、CacheLookupResult、ProviderSession、SegmentReadHandle、Progress | 建立 Provider 会话失败可重试 |
| InitLocalState | 为每个执行线程创建本地状态，绑定该线程负责的 Segment / 远端 Scanner 分片 | 线程级失败向 Global 状态上报 |
| Execute | 从 Cache 或远端取数，填充 `DataChunk`，返回 `OperatorResultType` | 检查 `cancelled`，取消时关闭远端 Scanner 与本地 Reader |

```
HBaseScanGlobalState { ScanPlan, CacheLookupResult, ProviderSession, SegmentReadHandle, Progress }
    +-- HBaseScanLocalState[Thread 1..N]   // 各自读取被分配的 Segment / Scanner 分片
```

Global 负责分片分配与资源协调，Local 负责单线程读取；Extension 不持有全局 Gateway 连接，按连接配置创建独立 Provider Session。

> 详见 [技术方案 §2.3](../tech_v0.1.md)。

## 4. Scan Planner / Executor / Pipeline 分工

| 组件 | 职责 | 关键输出 |
| --- | --- | --- |
| `ScanPlanner` | 汇总 Projection、Filter、RowKey Range 与一致性要求 | `ScanPlan` |
| `ScanRequestBuilder` | 翻译为统一请求（request_id、range、columns、时间戳、max_versions、batch_size） | `ScanRequest` |
| `ProjectionPlanner` | 计算 Required Columns 与 Output Columns | 列集合 |
| `PredicatePlanner` | 解析 WHERE，区分可下推与仅本地执行的谓词 | 过滤表达式 |
| `RowKeyRangePlanner` | 计算并归一化 RowKey 区间，处理开闭边界 | `RowKeyRange` 列表 |
| `ScanExecutor` | 驱动完整 Scan 流程，处理取消 | 结果流 / 错误 |
| `ScanPipeline` | 串联 Cache、Provider 与 DuckDB 执行，处理命中/缺失/合并 | `DataChunk` |
| `DataChunkConverter` | Arrow RecordBatch 到 DataChunk 的批量转换 | `DataChunk` |

`ScanPlan` 示意结构：

```cpp
struct ScanPlan {
    TableIdentifier table;
    vector<ColumnRef> projected_columns;   // 正式方案要求区分 Output / Required
    vector<FilterExpression> filters;
    vector<RowKeyRange> rowkey_ranges;
    bool require_rowkey;
    bool allow_cache;
    ScanConsistency consistency;
};
```

方案正文要求 `ScanPlan` 至少覆盖 TableIdentifier、RequiredColumns、OutputColumns、Filters、RowKeyRanges、ScanConsistency、CachePolicy；上面的早期示意与之字段命名尚未统一，最终字段集合待定（TBD）。

Projection Pushdown：只把 Required Columns 传给远端与 Cache，Output Columns 仅用于构造输出。例如 `SELECT name ... WHERE age > 30` 必须读取 `{name, age}`，只读 `name` 会让过滤失真。

Filter Pushdown：能否下推由 `ProviderCapabilities.supports_filter_pushdown` 决定；不下推时由 DuckDB 本地执行，但 Filter 列仍必须读入。支持的表达式与算子集合待定（TBD）。

RowKey Range 规划：`RowKey` 定义为 `std::vector<uint8_t>`，区间结构为：

```cpp
struct RowKeyRange {
    RowKey start, stop;
    bool include_start, include_stop;
};
```

VARCHAR 入参按 UTF-8 转字节，BLOB 入参保持原始字节；缓存覆盖比较必须与 HBase 使用相同的字节序规则，不能用字符串排序替代。

> 详见 [技术方案 §2.4](../tech_v0.1.md)、[§3.5](../tech_v0.1.md)、[§7.1](../tech_v0.1.md)、[§7.2](../tech_v0.1.md)。

## 5. DataChunk 与 Arrow 的转换要点

- 远端与 Cache 都以 Arrow 列为单位流转：`IScanStream::Next(ArrowRecordBatch &)` 逐批返回，`ICacheReader::Next(DataChunk &)` 面向执行层，`DataChunkConverter` 负责批量转换。
- 流式约束：Scan 不返回 `vector<DataChunk>`，禁止全量结果无界加载内存；`ArrowBatch` 携带 `ipc_payload`、`sequence_number`、`finished`、`row_count`。
- RowKey 按二进制处理，映射为 BLOB 语义，不做默认 UTF-8 解释。
- 缺失语义要区分：列存在但值为空、列不存在、RowKey 不存在、查询期间 RowKey 变化。HBase Cell 不存在应映射为 NULL，而不是判定 RowKey 不存在。
- 版本与时间戳：`ScanConsistency` 含 `min_timestamp`、`max_timestamp`、`max_versions`、`latest_only`；`max_versions > 1` 或指定时间范围的查询不能复用仅含最新版本的缓存。
- 补列结果按 RowKey 对齐后再拼接，RowKey 必须作为显式列参与合并。
- 具体 Arrow 类型到 DuckDB `LogicalType` 的映射表（含 DECIMAL、TIMESTAMP 精度等）来源未给出，待定（TBD），建议统一收敛在 `catalog/type_mapping.cpp`。

> 详见 [技术方案 §3.3](../tech_v0.1.md)、[§7.3](../tech_v0.1.md)、[§7.4](../tech_v0.1.md)。

## 6. 并发扫描、失败恢复与超时重试的职责边界

并发扫描：

- Global 状态负责把 RowKey 区间 / Segment 分配给各 Local 状态，避免线程间重叠读取。
- Cache 写入侧由 `CacheManager` 统一处理 Segment Lock、Column Lock、SingleFlight、Reader Reference Count、Generation CAS：相同缺失列的并发请求合并为一次补列，不同列可并行；提交用 Generation CAS 防止覆盖。
- Extension 只保证计划分发与状态隔离，不承担 Gateway 侧连接池与限流。

失败恢复：缓存写入分四阶段，失败按所在阶段处理。

| 阶段 | 失败处理 |
| --- | --- |
| INIT | 直接终止 |
| WRITING | 删除临时文件 |
| VALIDATING | 校验失败则 Abort |
| COMMITTED | 元数据已生效，不回滚已发布 Segment |

查询取消必须同时关闭远端 Scanner 与本地文件 Reader；`ScanExecutionContext` 用 `atomic<bool> cancelled` 传播取消。

超时与重试（Extension 侧只做策略判定，不自行重放数据流）：

| 操作 | 策略 |
| --- | --- |
| Schema 查询 | 可有限重试 |
| Scan 建立 | 建立失败可重试 |
| Scan 流读取 | 谨慎，避免重复返回数据 |
| Batch Get | 按 RowKey 批次重试 |
| Cache 写入 | 允许 Abort 后重试 |
| Cancel | 幂等 |

已返回部分结果的 Scan 不能从头重试再拼接到旧数据；断点续传需要最后处理的 RowKey 与稳定扫描语义，第一版不实现。超时参数来源为连接 JSON 的 `timeout_ms`（示例 30000）与 Gateway 的 `rpc_timeout_ms`（示例 30000），Extension 侧 deadline 的具体传播方式待定（TBD）。

> 详见 [技术方案 §7.5](../tech_v0.1.md)、[§7.6](../tech_v0.1.md)、[§8.3](../tech_v0.1.md)。

## 6.1 扫描执行实现（M1）

- `RemoteScanStream`：后台线程调用 `Provider::Scan`，把 Arrow 批推入**有界队列**（默认 2 批），主线程按需消费，内存不随结果集增长。
- `DecodeArrowIpc`：解码单批 Arrow IPC（UTF8 列 + validity），映射为字符串列；`DataChunk` 用 `FlatVector` + `StringVector::AddString` 填充。
- 单查询串行消费（`MaxThreads()=1`）；并行扫描在 M2 按 RowKey 区间切分。
- 取消：查询中止时 `RemoteScanStream::Close()` 通知工作线程退出；gRPC 侧的 `Cancel` 在 M2 接入（当前依赖连接关闭）。

## 7. Gateway 可用性与连接生命周期

Extension 与 Gateway 是两个独立进程，Gateway 不可用时的行为必须明确，否则会出现“启动顺序依赖”和难以恢复的失败。

| 阶段 | 是否访问 Gateway | 失败行为 | 原因 |
| --- | --- | --- | --- |
| `LOAD hbase` | 否 | 不失败 | LOAD 失败会被 DuckDB 缓存，Gateway 后启动将无法恢复；同进程可能注册多个 Gateway |
| `hbase_connect()` | 是 | 抛错并**不注册**该连接 | 建连即握手：`GetCapabilities` + `Health`（带 deadline 与有限重试），不可用则明确告知用户 |
| 查询（`hbase_scan` 等） | 是 | 快速失败，不静默降级 | 已有连接在 Gateway 重启后，允许**一次**重连尝试；仍失败则报错并提示重新 `hbase_connect` |
| `LOAD` 之后的空闲期 | 否 | —— | 不做后台心跳，避免无谓连接与线程 |

因此，**“Extension 启动时探测 Gateway 并拒绝加载”是明确不采用的做法**：

1. 启动顺序不该成为约束：用户的 DuckDB 脚本可能先 `LOAD`，Gateway 稍后启动。
2. LOAD 失败是粘性的：DuckDB 会缓存加载结果，用户只能重启进程。
3. 一个 DuckDB 进程可以同时连接多个 Gateway，“全局不可用”没有意义；可用性是**按连接**判断的。

对用户的可见语义是：`LOAD` 永远成功；`hbase_connect` 在 Gateway 不可用时立即报错（错误信息包含 endpoint 与原因）；已建立的连接在网关故障后，后续查询报错而不是返回空结果。

## 7. 关键类关系

```text
HBaseExtension
  -> HBaseFunctionRegistry
       -> HBaseScanFunction / HBaseGetFunction
            -> ScanPlanner::BuildPlan()
                 -> ScanExecutor::Execute() / Cancel()
                      -> ScanPipeline
                           -> ICacheManager           (Lookup / OpenReader / BeginWrite)
                                -> ICacheStorage      (OpenReader / CreateWriter)
                           -> IRemoteScanProvider     (Scan / BatchGet / GetSchema / Cancel)
                                -> IScanStream        (Arrow RecordBatch)
                                -> IGetStream         (Arrow RecordBatch)
                           -> DataChunkConverter      (Arrow -> DataChunk)
                                -> DataChunk
```

`ScanPipeline` 位于 `ScanExecutor` 与上述两个接口之间，负责命中、缺失、补列与合并的顺序编排；`CacheManager` 依赖 `ICacheStorage`，不感知 Gateway 类型。

> 详见 [技术方案 §2.5](../tech_v0.1.md)、[§6.3](../tech_v0.1.md)。
