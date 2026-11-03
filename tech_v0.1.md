# DuckDB-HBase Extension 完整技术方案

Technical Design Document · V1.0 · 2026-11-01

这次需要把方案从架构设计推进到可以指导实际开发的工程设计，包括 C++ Extension、Gateway 抽象、Java 适配器、缓存引擎、用户 API、部署安装和工程分包。

我建议将项目正式定位为：

DuckDB-HBase：一个支持多种 HBase 接入协议、具备本地列式 Scan Cache 的 DuckDB 外部数据源扩展。

有三个架构决策需要先明确：

1. Extension 不直接依赖某一种 HBase Gateway。 Extension 只依赖统一的 Remote Scan Provider 协议，Gateway 可以替换成 Java Gateway、HBase REST、Thrift 或其他实现。
2. Cache Engine 属于 Extension 核心能力，而不是 Gateway 的能力。 不同 Gateway 只负责远端数据访问，缓存策略、数据合并、命中判断都由 Extension 统一管理。
3. 用户只需要使用 DuckDB SQL 和少量配置 API。 用户不需要理解 Gateway 内部协议，也不需要手动管理缓存 Segment。

整体采用以下技术栈：

| 层次        | 技术                       | 职责                     |
| --------- | ------------------------ | ---------------------- |
| SQL 层     | DuckDB C++ Extension API | SQL 接入、Scan 执行         |
| 执行层       | DuckDB Table Function    | Projection、Filter、并行执行 |
| Cache 层   | C++ + Parquet + Metadata | Scan 缓存、列复用、补列         |
| 协议层       | Protobuf + Arrow IPC     | 统一远程数据协议               |
| Gateway 层 | Java / HTTP / gRPC       | HBase 访问适配             |
| 数据源层      | HBase 2.x                | 实际数据存储                 |
| 构建层       | CMake + Maven            | Extension 与 Gateway 构建 |

本方案保持之前确定的约束：自建 Hadoop/HBase 2.x 集群、独立 Gateway、优先实现 SQL Table Function、不实现 Catalog，第一阶段以只读查询为主。

## 一、总体架构与模块边界

### 1.1 架构分层

整个项目分为四个独立层次：

\#chatgpt-mermaid-\_r_14s\_{font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:16px;fill:rgb(13, 13, 13);}@keyframes edge-animation-frame{from{stroke-dashoffset:0;}}@keyframes dash{to{stroke-dashoffset:0;}}#chatgpt-mermaid-\_r_14s\_ .edge-animation-slow{stroke-dasharray:9,5!important;stroke-dashoffset:900;animation:dash 50s linear infinite;stroke-linecap:round;}#chatgpt-mermaid-\_r_14s\_ .edge-animation-fast{stroke-dasharray:9,5!important;stroke-dashoffset:900;animation:dash 20s linear infinite;stroke-linecap:round;}#chatgpt-mermaid-\_r_14s\_ .error-icon{fill:rgb(243, 243, 243);}#chatgpt-mermaid-\_r_14s\_ .error-text{fill:rgb(13, 13, 13);stroke:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_14s\_ .edge-thickness-normal{stroke-width:1px;}#chatgpt-mermaid-\_r_14s\_ .edge-thickness-thick{stroke-width:3.5px;}#chatgpt-mermaid-\_r_14s\_ .edge-pattern-solid{stroke-dasharray:0;}#chatgpt-mermaid-\_r_14s\_ .edge-thickness-invisible{stroke-width:0;fill:none;}#chatgpt-mermaid-\_r_14s\_ .edge-pattern-dashed{stroke-dasharray:3;}#chatgpt-mermaid-\_r_14s\_ .edge-pattern-dotted{stroke-dasharray:2;}#chatgpt-mermaid-\_r_14s\_ .marker{fill:rgb(143, 143, 143);stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_14s\_ .marker.cross{stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_14s\_ svg{font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:16px;}#chatgpt-mermaid-\_r_14s\_ p{margin:0;}#chatgpt-mermaid-\_r_14s\_ .label{font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_14s\_ .cluster-label text{fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_14s\_ .cluster-label span{color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_14s\_ .cluster-label span p{background-color:transparent;}#chatgpt-mermaid-\_r_14s\_ .label text,#chatgpt-mermaid-\_r_14s\_ span{fill:rgb(13, 13, 13);color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_14s\_ .node rect,#chatgpt-mermaid-\_r_14s\_ .node circle,#chatgpt-mermaid-\_r_14s\_ .node ellipse,#chatgpt-mermaid-\_r_14s\_ .node polygon,#chatgpt-mermaid-\_r_14s\_ .node path{fill:rgb(222, 234, 251);stroke:rgb(83, 154, 248);stroke-width:1px;}#chatgpt-mermaid-\_r_14s\_ .rough-node .label text,#chatgpt-mermaid-\_r_14s\_ .node .label text,#chatgpt-mermaid-\_r_14s\_ .image-shape .label,#chatgpt-mermaid-\_r_14s\_ .icon-shape .label{text-anchor:middle;}#chatgpt-mermaid-\_r_14s\_ .node .katex path{fill:#000;stroke:#000;stroke-width:1px;}#chatgpt-mermaid-\_r_14s\_ .rough-node .label,#chatgpt-mermaid-\_r_14s\_ .node .label,#chatgpt-mermaid-\_r_14s\_ .image-shape .label,#chatgpt-mermaid-\_r_14s\_ .icon-shape .label{text-align:center;}#chatgpt-mermaid-\_r_14s\_ .node.clickable{cursor:pointer;}#chatgpt-mermaid-\_r_14s\_ .root .anchor path{fill:rgb(143, 143, 143)!important;stroke-width:0;stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_14s\_ .arrowheadPath{fill:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_14s\_ .edgePath .path{stroke:rgb(143, 143, 143);stroke-width:1px;}#chatgpt-mermaid-\_r_14s\_ .flowchart-link{stroke:rgb(143, 143, 143);fill:none;}#chatgpt-mermaid-\_r_14s\_ .edgeLabel{background-color:rgb(252, 252, 252);text-align:center;}#chatgpt-mermaid-\_r_14s\_ .edgeLabel p{background-color:rgb(252, 252, 252);}#chatgpt-mermaid-\_r_14s\_ .edgeLabel rect{opacity:0.5;background-color:rgb(252, 252, 252);fill:rgb(252, 252, 252);}#chatgpt-mermaid-\_r_14s\_ .labelBkg{background-color:rgba(252, 252, 252, 0.5);}#chatgpt-mermaid-\_r_14s\_ .cluster rect{fill:rgb(243, 243, 243);stroke:rgba(0, 0, 0, 0.1);stroke-width:1px;}#chatgpt-mermaid-\_r_14s\_ .cluster text{fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_14s\_ .cluster span{color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_14s\_ div.mermaidTooltip{position:absolute;text-align:center;max-width:200px;padding:2px;font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:12px;background:rgb(243, 243, 243);border:1px solid rgba(0, 0, 0, 0.1);border-radius:2px;pointer-events:none;z-index:100;}#chatgpt-mermaid-\_r_14s\_ .flowchartTitleText{text-anchor:middle;font-size:18px;fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_14s\_ rect.text{fill:none;stroke-width:0;}#chatgpt-mermaid-\_r_14s\_ .icon-shape,#chatgpt-mermaid-\_r_14s\_ .image-shape{background-color:rgb(252, 252, 252);text-align:center;}#chatgpt-mermaid-\_r_14s\_ .icon-shape p,#chatgpt-mermaid-\_r_14s\_ .image-shape p{background-color:rgb(252, 252, 252);padding:2px;}#chatgpt-mermaid-\_r_14s\_ .icon-shape .label rect,#chatgpt-mermaid-\_r_14s\_ .image-shape .label rect{opacity:0.5;background-color:rgb(252, 252, 252);fill:rgb(252, 252, 252);}#chatgpt-mermaid-\_r_14s\_ .label-icon{display:inline-block;height:1em;overflow:visible;vertical-align:-0.125em;}#chatgpt-mermaid-\_r_14s\_ .node .label-icon path{fill:currentColor;stroke:revert;stroke-width:revert;}#chatgpt-mermaid-\_r_14s\_ .node .neo-node{stroke:rgb(83, 154, 248);}#chatgpt-mermaid-\_r_14s\_ [data-look="neo"].node rect,#chatgpt-mermaid-\_r_14s\_ [data-look="neo"].cluster rect,#chatgpt-mermaid-\_r_14s\_ [data-look="neo"].node polygon{stroke:url(#chatgpt-mermaid-\_r_14s\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_14s\_ [data-look="neo"].swimlane.cluster rect{filter:none;}#chatgpt-mermaid-\_r_14s\_ [data-look="neo"].node path{stroke:url(#chatgpt-mermaid-\_r_14s\_-gradient);stroke-width:1px;}#chatgpt-mermaid-\_r_14s\_ [data-look="neo"].node .outer-path{filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_14s\_ [data-look="neo"].node .neo-line path{stroke:rgb(83, 154, 248);filter:none;}#chatgpt-mermaid-\_r_14s\_ [data-look="neo"].node circle{stroke:url(#chatgpt-mermaid-\_r_14s\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_14s\_ [data-look="neo"].node circle .state-start{fill:#000000;}#chatgpt-mermaid-\_r_14s\_ [data-look="neo"].icon-shape .icon{fill:url(#chatgpt-mermaid-\_r_14s\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_14s\_ [data-look="neo"].icon-shape .icon-neo path{stroke:url(#chatgpt-mermaid-\_r_14s\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_14s\_ .node text{font-size:14px;font-weight:600;letter-spacing:normal;fill:rgb(0, 79, 153);}#chatgpt-mermaid-\_r_14s\_ .edgeLabels text{font-size:13px;font-weight:600;letter-spacing:-0.08px;fill:rgb(0, 79, 153);}#chatgpt-mermaid-\_r_14s\_ .node tspan[font-weight="normal"],#chatgpt-mermaid-\_r_14s\_ .edgeLabels tspan[font-weight="normal"]{font-weight:600;}#chatgpt-mermaid-\_r_14s\_ .edgeLabel .label rect{opacity:1;rx:13px;ry:13px;fill:rgb(245, 250, 255);stroke:rgb(206, 219, 229);stroke-width:1px;}#chatgpt-mermaid-\_r_14s\_ .node rect,#chatgpt-mermaid-\_r_14s\_ .node circle,#chatgpt-mermaid-\_r_14s\_ .node ellipse,#chatgpt-mermaid-\_r_14s\_ .node polygon,#chatgpt-mermaid-\_r_14s\_ .node path{fill:rgb(229, 243, 255);stroke:rgba(0, 0, 0, 0.1);stroke-width:1px;}#chatgpt-mermaid-\_r_14s\_ .node rect{rx:16px;ry:16px;}#chatgpt-mermaid-\_r_14s\_ .mermaid-decision .label-container{fill:rgb(245, 250, 255);stroke:rgb(206, 219, 229);stroke-dasharray:2,2;}#chatgpt-mermaid-\_r_14s\_ .edgePaths .flowchart-link{stroke:rgb(143, 143, 143);stroke-width:1px;stroke-linecap:round;stroke-linejoin:round;}#chatgpt-mermaid-\_r_14s\_ .marker{fill:rgb(143, 143, 143);stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_14s\_ :root{--mermaid-font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";}HBase InfrastructureGateway / Access AdapterRemote Access ProtocolDuckDB-HBase Extension（C++）用户侧HBase 2.xGateway SPINative Java HBase AdapterHBase REST AdapterHBase Thrift AdapterPhoenix JDBC AdapterRemote Provider ProtocolProtocol AdapterTable Function APIScan PlannerScan ExecutorCache ManagerCache StorageCache IndexGateway ClientDuckDB SQL / Python API

其中有一个重要边界：

- Extension 只关心统一的 `RemoteScanProvider`。
- Gateway SPI 负责定义远端访问能力。
- 不同 Gateway Adapter 实现 SPI。
- HBase 原生 Java Client 是默认实现。
- REST、Thrift、Phoenix 是可插拔实现。

这样，无论底层使用什么形式接入 HBase，DuckDB 的 SQL、Cache 和 Scan 执行逻辑都不需要改变。

### 1.2 核心依赖关系

依赖方向

duckdb_hbase_extension

只依赖统一协议、Arrow、缓存存储接口

Remote Provider Protocol

定义 Scan / Get / Capabilities / Schema

Gateway Adapter SPI

实现协议转换、请求路由、连接管理

Native Java

默认

HBase Java Client

REST

HTTP API

Thrift

Thrift Client

Phoenix

JDBC Adapter

Extension 不直接依赖任何具体 Gateway 实现。

## 二、Extension 核心模块详细设计

Extension 是项目的主体，采用 C++ 实现，建议按照领域职责划分为八个模块。

### 2.1 模块划分

| 模块        | 核心职责                             | 是否核心 |
| --------- | -------------------------------- | ---- |
| extension | DuckDB 扩展注册、初始化                  | 是    |
| sql       | Table Function、SQL API           | 是    |
| scan      | Scan Planning、执行和数据转换            | 是    |
| cache     | 缓存判断、读写、合并                       | 是    |
| storage   | Parquet、内存及 Metadata 存储          | 是    |
| provider  | Gateway Client、协议适配              | 是    |
| catalog   | Schema 和列信息映射，不实现 DuckDB Catalog | 辅助   |
| common    | 配置、异常、日志、类型定义                    | 是    |

这里需要特别区分 `catalog` 模块和 DuckDB Catalog。

我们不实现 DuckDB 原生 Catalog 注册机制，只实现一个轻量的 HBase Schema Metadata Provider，负责获取表的 Family、Qualifier 和类型映射。

### 2.2 extension 模块

职责：

- Extension 加载。
- 注册 SQL Table Function。
- 注册配置项。
- 注册管理函数。
- 管理 Extension 生命周期。

主要类：

| 类                       | 职责              |
| ----------------------- | --------------- |
| `HBaseExtension`        | Extension 入口    |
| `HBaseExtensionConfig`  | 全局配置            |
| `HBaseExtensionState`   | Extension 运行时状态 |
| `HBaseFunctionRegistry` | SQL 函数注册        |

核心入口：

```
class HBaseExtension : public Extension {
public:
    void Load(ExtensionLoader &loader) override;
    std::string Name() override;
    std::string Version() const override;
};
```

加载时完成：

```
HBaseExtension::Load()
    |
    +-- RegisterHBaseScanFunction()
    +-- RegisterHBaseGetFunction()
    +-- RegisterConnectionFunctions()
    +-- RegisterCacheFunctions()
    +-- RegisterConfiguration()
```

建议 Extension 不直接持有一个全局 Gateway 连接，而是根据连接配置创建独立的 Provider Session。

### 2.3 sql 模块

这一层负责把 SQL 转换成统一的 ScanRequest。

建议第一阶段暴露以下 Table Function：

| API                   | 功能         | 优先级 |
| --------------------- | ---------- | --- |
| `hbase_scan()`        | 范围 Scan    | P0  |
| `hbase_get()`         | RowKey Get | P1  |
| `hbase_tables()`      | 查询表列表      | P1  |
| `hbase_columns()`     | 查询列信息      | P1  |
| `hbase_cache_stats()` | 缓存统计       | P0  |
| `hbase_cache_clear()` | 清理缓存       | P0  |
| `hbase_connections()` | 查看连接       | P1  |

核心类：

```
class HBaseScanFunction {
public:
    static unique_ptr<FunctionData> Bind(
        ClientContext &context,
        TableFunctionBindInput &input,
        vector<LogicalType> &return_types,
        vector<string> &names
    );

    static unique_ptr<GlobalTableFunctionState>
    InitGlobalState(
        ClientContext &context,
        TableFunctionInitInput &input
    );

    static unique_ptr<LocalTableFunctionState>
    InitLocalState(
        ExecutionContext &context,
        TableFunctionInitInput &input,
        GlobalTableFunctionState *global_state
    );

    static OperatorResultType Execute(
        ExecutionContext &context,
        TableFunctionInput &data,
        DataChunk &output
    );
};
```

以上是接口职责示意，具体签名应按项目锁定的 DuckDB 版本校准。

关键设计：

- `Bind`：解析参数、获取 Schema、构建输出列。
- `InitGlobalState`：建立本次查询的全局执行状态。
- `InitLocalState`：为每个执行线程建立本地状态。
- `Execute`：读取 Cache 或远端数据，填充 DuckDB DataChunk。

建议采用 Global / Local 两级执行状态：

```
HBaseScanGlobalState
    |
    +-- ScanPlan
    +-- CacheLookupResult
    +-- ProviderSession
    +-- SegmentReadHandle
    +-- Progress
    |
    +-- HBaseScanLocalState[Thread 1]
    +-- HBaseScanLocalState[Thread 2]
    +-- HBaseScanLocalState[Thread N]
```

Global 状态负责任务分配和资源协调，Local 状态负责各线程的数据读取。

### 2.4 scan 模块

这是 SQL 与远程数据源之间的执行层。

主要类：

| 类                    | 职责                          |
| -------------------- | --------------------------- |
| `ScanPlanner`        | 构建 ScanPlan                 |
| `ScanRequestBuilder` | 构建统一 ScanRequest            |
| `ProjectionPlanner`  | 计算所需列                       |
| `PredicatePlanner`   | 解析过滤条件                      |
| `RowKeyRangePlanner` | 计算 RowKey 范围                |
| `ScanExecutor`       | 执行完整 Scan 流程                |
| `ScanPipeline`       | 串联 Cache、Provider 和 DuckDB  |
| `DataChunkConverter` | Arrow 到 DuckDB DataChunk 转换 |

核心数据结构：

```
struct ScanPlan {
    TableIdentifier table;

    vector<ColumnRef> projected_columns;
    vector<FilterExpression> filters;

    vector<RowKeyRange> rowkey_ranges;

    bool require_rowkey;
    bool allow_cache;

    ScanConsistency consistency;
};

struct ScanExecutionContext {
    ScanPlan plan;
    CacheLookupResult cache_result;

    shared_ptr<IRemoteScanProvider> provider;

    atomic<bool> cancelled;
};
```

这里有一个关键问题：Projection 和 Filter 必须分开。

例如：

```
SELECT name
FROM hbase_scan('prod', 'customer_profile')
WHERE age > 30;
```

实际需要读取的列：

```
Required Columns = {name, age}
Output Columns   = {name}
```

如果只读取 `name`，就无法正确执行 `age > 30`。

### 2.5 scan 执行流程

\#chatgpt-mermaid-\_r_16p\_{font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:16px;fill:rgb(13, 13, 13);}@keyframes edge-animation-frame{from{stroke-dashoffset:0;}}@keyframes dash{to{stroke-dashoffset:0;}}#chatgpt-mermaid-\_r_16p\_ .edge-animation-slow{stroke-dasharray:9,5!important;stroke-dashoffset:900;animation:dash 50s linear infinite;stroke-linecap:round;}#chatgpt-mermaid-\_r_16p\_ .edge-animation-fast{stroke-dasharray:9,5!important;stroke-dashoffset:900;animation:dash 20s linear infinite;stroke-linecap:round;}#chatgpt-mermaid-\_r_16p\_ .error-icon{fill:rgb(243, 243, 243);}#chatgpt-mermaid-\_r_16p\_ .error-text{fill:rgb(13, 13, 13);stroke:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_16p\_ .edge-thickness-normal{stroke-width:1px;}#chatgpt-mermaid-\_r_16p\_ .edge-thickness-thick{stroke-width:3.5px;}#chatgpt-mermaid-\_r_16p\_ .edge-pattern-solid{stroke-dasharray:0;}#chatgpt-mermaid-\_r_16p\_ .edge-thickness-invisible{stroke-width:0;fill:none;}#chatgpt-mermaid-\_r_16p\_ .edge-pattern-dashed{stroke-dasharray:3;}#chatgpt-mermaid-\_r_16p\_ .edge-pattern-dotted{stroke-dasharray:2;}#chatgpt-mermaid-\_r_16p\_ .marker{fill:rgb(143, 143, 143);stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_16p\_ .marker.cross{stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_16p\_ svg{font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:16px;}#chatgpt-mermaid-\_r_16p\_ p{margin:0;}#chatgpt-mermaid-\_r_16p\_ .label{font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_16p\_ .cluster-label text{fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_16p\_ .cluster-label span{color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_16p\_ .cluster-label span p{background-color:transparent;}#chatgpt-mermaid-\_r_16p\_ .label text,#chatgpt-mermaid-\_r_16p\_ span{fill:rgb(13, 13, 13);color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_16p\_ .node rect,#chatgpt-mermaid-\_r_16p\_ .node circle,#chatgpt-mermaid-\_r_16p\_ .node ellipse,#chatgpt-mermaid-\_r_16p\_ .node polygon,#chatgpt-mermaid-\_r_16p\_ .node path{fill:rgb(222, 234, 251);stroke:rgb(83, 154, 248);stroke-width:1px;}#chatgpt-mermaid-\_r_16p\_ .rough-node .label text,#chatgpt-mermaid-\_r_16p\_ .node .label text,#chatgpt-mermaid-\_r_16p\_ .image-shape .label,#chatgpt-mermaid-\_r_16p\_ .icon-shape .label{text-anchor:middle;}#chatgpt-mermaid-\_r_16p\_ .node .katex path{fill:#000;stroke:#000;stroke-width:1px;}#chatgpt-mermaid-\_r_16p\_ .rough-node .label,#chatgpt-mermaid-\_r_16p\_ .node .label,#chatgpt-mermaid-\_r_16p\_ .image-shape .label,#chatgpt-mermaid-\_r_16p\_ .icon-shape .label{text-align:center;}#chatgpt-mermaid-\_r_16p\_ .node.clickable{cursor:pointer;}#chatgpt-mermaid-\_r_16p\_ .root .anchor path{fill:rgb(143, 143, 143)!important;stroke-width:0;stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_16p\_ .arrowheadPath{fill:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_16p\_ .edgePath .path{stroke:rgb(143, 143, 143);stroke-width:1px;}#chatgpt-mermaid-\_r_16p\_ .flowchart-link{stroke:rgb(143, 143, 143);fill:none;}#chatgpt-mermaid-\_r_16p\_ .edgeLabel{background-color:rgb(252, 252, 252);text-align:center;}#chatgpt-mermaid-\_r_16p\_ .edgeLabel p{background-color:rgb(252, 252, 252);}#chatgpt-mermaid-\_r_16p\_ .edgeLabel rect{opacity:0.5;background-color:rgb(252, 252, 252);fill:rgb(252, 252, 252);}#chatgpt-mermaid-\_r_16p\_ .labelBkg{background-color:rgba(252, 252, 252, 0.5);}#chatgpt-mermaid-\_r_16p\_ .cluster rect{fill:rgb(243, 243, 243);stroke:rgba(0, 0, 0, 0.1);stroke-width:1px;}#chatgpt-mermaid-\_r_16p\_ .cluster text{fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_16p\_ .cluster span{color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_16p\_ div.mermaidTooltip{position:absolute;text-align:center;max-width:200px;padding:2px;font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:12px;background:rgb(243, 243, 243);border:1px solid rgba(0, 0, 0, 0.1);border-radius:2px;pointer-events:none;z-index:100;}#chatgpt-mermaid-\_r_16p\_ .flowchartTitleText{text-anchor:middle;font-size:18px;fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_16p\_ rect.text{fill:none;stroke-width:0;}#chatgpt-mermaid-\_r_16p\_ .icon-shape,#chatgpt-mermaid-\_r_16p\_ .image-shape{background-color:rgb(252, 252, 252);text-align:center;}#chatgpt-mermaid-\_r_16p\_ .icon-shape p,#chatgpt-mermaid-\_r_16p\_ .image-shape p{background-color:rgb(252, 252, 252);padding:2px;}#chatgpt-mermaid-\_r_16p\_ .icon-shape .label rect,#chatgpt-mermaid-\_r_16p\_ .image-shape .label rect{opacity:0.5;background-color:rgb(252, 252, 252);fill:rgb(252, 252, 252);}#chatgpt-mermaid-\_r_16p\_ .label-icon{display:inline-block;height:1em;overflow:visible;vertical-align:-0.125em;}#chatgpt-mermaid-\_r_16p\_ .node .label-icon path{fill:currentColor;stroke:revert;stroke-width:revert;}#chatgpt-mermaid-\_r_16p\_ .node .neo-node{stroke:rgb(83, 154, 248);}#chatgpt-mermaid-\_r_16p\_ [data-look="neo"].node rect,#chatgpt-mermaid-\_r_16p\_ [data-look="neo"].cluster rect,#chatgpt-mermaid-\_r_16p\_ [data-look="neo"].node polygon{stroke:url(#chatgpt-mermaid-\_r_16p\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_16p\_ [data-look="neo"].swimlane.cluster rect{filter:none;}#chatgpt-mermaid-\_r_16p\_ [data-look="neo"].node path{stroke:url(#chatgpt-mermaid-\_r_16p\_-gradient);stroke-width:1px;}#chatgpt-mermaid-\_r_16p\_ [data-look="neo"].node .outer-path{filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_16p\_ [data-look="neo"].node .neo-line path{stroke:rgb(83, 154, 248);filter:none;}#chatgpt-mermaid-\_r_16p\_ [data-look="neo"].node circle{stroke:url(#chatgpt-mermaid-\_r_16p\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_16p\_ [data-look="neo"].node circle .state-start{fill:#000000;}#chatgpt-mermaid-\_r_16p\_ [data-look="neo"].icon-shape .icon{fill:url(#chatgpt-mermaid-\_r_16p\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_16p\_ [data-look="neo"].icon-shape .icon-neo path{stroke:url(#chatgpt-mermaid-\_r_16p\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_16p\_ .node text{font-size:14px;font-weight:600;letter-spacing:normal;fill:rgb(0, 79, 153);}#chatgpt-mermaid-\_r_16p\_ .edgeLabels text{font-size:13px;font-weight:600;letter-spacing:-0.08px;fill:rgb(0, 79, 153);}#chatgpt-mermaid-\_r_16p\_ .node tspan[font-weight="normal"],#chatgpt-mermaid-\_r_16p\_ .edgeLabels tspan[font-weight="normal"]{font-weight:600;}#chatgpt-mermaid-\_r_16p\_ .edgeLabel .label rect{opacity:1;rx:13px;ry:13px;fill:rgb(245, 250, 255);stroke:rgb(206, 219, 229);stroke-width:1px;}#chatgpt-mermaid-\_r_16p\_ .node rect,#chatgpt-mermaid-\_r_16p\_ .node circle,#chatgpt-mermaid-\_r_16p\_ .node ellipse,#chatgpt-mermaid-\_r_16p\_ .node polygon,#chatgpt-mermaid-\_r_16p\_ .node path{fill:rgb(229, 243, 255);stroke:rgba(0, 0, 0, 0.1);stroke-width:1px;}#chatgpt-mermaid-\_r_16p\_ .node rect{rx:16px;ry:16px;}#chatgpt-mermaid-\_r_16p\_ .mermaid-decision .label-container{fill:rgb(245, 250, 255);stroke:rgb(206, 219, 229);stroke-dasharray:2,2;}#chatgpt-mermaid-\_r_16p\_ .edgePaths .flowchart-link{stroke:rgb(143, 143, 143);stroke-width:1px;stroke-linecap:round;stroke-linejoin:round;}#chatgpt-mermaid-\_r_16p\_ .marker{fill:rgb(143, 143, 143);stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_16p\_ :root{--mermaid-font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";}SQL Bind获取 Schema生成 ScanPlan计算 Required Columns计算 RowKey RangeCache LookupCache 状态Cache Scan读取已缓存数据Remote Scan计算缺失列和范围Remote Get / ScanRowKey MergeCache CommitArrow Batch StreamCache WriteDataChunk ConverterDuckDB 执行后续算子FULL HITPARTIAL HITMISS

### 2.6 cache 模块

Cache 需要独立于 HBase 和 Gateway。

建议将其拆成五个子模块：

| 子模块          | 类                          | 职责              |
| ------------ | -------------------------- | --------------- |
| lookup       | `CacheLookupEngine`        | 查询覆盖情况          |
| index        | `CacheCoverageIndex`       | 维护 Segment 覆盖索引 |
| segment      | `CacheSegmentManager`      | 管理 Segment 生命周期 |
| merger       | `CacheMerger`              | 根据 RowKey 合并数据  |
| invalidation | `CacheInvalidationManager` | TTL、手动失效、CDC    |

核心接口：

```
class ICacheManager {
public:
    virtual CacheLookupResult Lookup(
        const ScanPlan &plan
    ) = 0;

    virtual unique_ptr<ICacheReader> OpenReader(
        const CacheLookupResult &result
    ) = 0;

    virtual unique_ptr<ICacheWriter> BeginWrite(
        const CacheWritePlan &plan
    ) = 0;

    virtual void Invalidate(
        const TableIdentifier &table,
        const optional<RowKeyRange> &range
    ) = 0;

    virtual CacheStatistics GetStatistics() = 0;

    virtual ~ICacheManager() = default;
};
```

Reader / Writer 单独抽象：

```
class ICacheReader {
public:
    virtual bool Next(DataChunk &chunk) = 0;
    virtual void Close() = 0;
    virtual ~ICacheReader() = default;
};

class ICacheWriter {
public:
    virtual void Append(const DataChunk &chunk) = 0;
    virtual void Commit() = 0;
    virtual void Abort() = 0;
    virtual ~ICacheWriter() = default;
};
```

建议采用以下 Cache 内部结构：

```
CacheManager
    |
    +-- CacheLookupEngine
    |      |
    |      +-- Coverage Index
    |      +-- Segment Index
    |
    +-- CacheSegmentManager
    |      |
    |      +-- Segment Reader
    |      +-- Segment Writer
    |
    +-- CacheMerger
    |
    +-- CacheInvalidationManager
    |
    +-- CacheEvictionManager
```

#### 一个重要的实现修正：列覆盖不能只依赖一个全局集合

例如：

```
Segment A:
  RowKey 0000-1000
  Columns: name, age

Segment B:
  RowKey 1000-2000
  Columns: name, city
```

查询：

```
SELECT name, age, city
FROM customer_profile;
```

不是简单的 Partial Hit，而是不同范围具有不同缺失列。

因此 Lookup 必须返回分段覆盖计划：

```
struct SegmentCoverage {
    string segment_id;
    RowKeyRange range;

    vector<ColumnRef> available_columns;
    vector<ColumnRef> missing_columns;
};

struct CacheLookupResult {
    CacheStatus status;
    vector<SegmentCoverage> segments;
    vector<RowKeyRange> uncached_ranges;
};
```

这能避免整个 Cache 系统被简单的 Full / Partial / Miss 三分类限制。

### 2.7 storage 模块

建议定义统一存储接口：

```
class ICacheStorage {
public:
    virtual unique_ptr<ICacheReader> OpenReader(
        const SegmentDescriptor &segment,
        const vector<ColumnRef> &columns
    ) = 0;

    virtual unique_ptr<ICacheWriter> CreateWriter(
        const SegmentDescriptor &segment
    ) = 0;

    virtual void DeleteSegment(
        const string &segment_id
    ) = 0;

    virtual ~ICacheStorage() = default;
};
```

第一版实现：

- `ParquetCacheStorage`：持久化。
- `MemoryCacheStorage`：热点缓存。

Metadata 建议使用独立的小型本地数据库，例如 SQLite，避免自行实现复杂的持久化索引。

建议的元数据表：

```
CREATE TABLE cache_segment (
    segment_id VARCHAR PRIMARY KEY,
    table_id VARCHAR,
    start_row BLOB,
    stop_row BLOB,
    row_count BIGINT,
    created_at BIGINT,
    expires_at BIGINT,
    generation BIGINT,
    file_path VARCHAR,
    status VARCHAR
);

CREATE TABLE cache_segment_column (
    segment_id VARCHAR,
    family VARCHAR,
    qualifier VARCHAR,
    logical_type VARCHAR,
    PRIMARY KEY(segment_id, family, qualifier)
);
```

SQLite 负责元数据，Parquet 负责实际数据。两者之间通过 staging + 原子 Metadata Commit 保证可恢复性。

### 2.8 provider 模块

这是 Extension 和 Gateway 之间唯一允许的依赖边界。

不允许：

```
// 不推荐
class HBaseScanExecutor {
    GrpcClient grpc_client;
};
```

也不允许：

```
class HBaseScanExecutor {
    RestClient rest_client;
};
```

推荐：

```
class HBaseScanExecutor {
    shared_ptr<IRemoteScanProvider> provider;
};
```

provider 负责：

- Provider Session 创建。
- 能力协商。
- Scan。
- Batch Get。
- Schema Metadata。
- 取消任务。
- 远端错误映射。

具体接口将在第三章展开。

## 三、Gateway 抽象与多接入方式适配设计

这一部分是本次架构升级的重点。

### 3.1 为什么需要 Gateway 抽象？

HBase 有多种接入方式：

| 接入方式               | 技术实现           | 优势        | 局限              | 定位   |
| ------------------ | -------------- | --------- | --------------- | ---- |
| Native Java Client | HBase Java API | 能力完整、性能好  | 需要 Java Gateway | 默认方案 |
| HBase REST         | HTTP REST      | 部署简单、语言无关 | 数据传输效率较低        | 兼容方案 |
| HBase Thrift       | Thrift RPC     | 多语言支持     | 能力和版本兼容需验证      | 兼容方案 |
| Phoenix            | JDBC           | SQL 查询体验好 | 与原生 Scan 语义不同   | 特殊适配 |
| Custom Gateway     | 自定义 RPC / HTTP | 可接入已有数据平台 | 需要实现协议          | 扩展方案 |

这里需要强调：

Phoenix 并不是 HBase 原生 Scan 的等价替代。它提供 SQL 层访问能力，可能具有独立的事务、索引、视图及查询语义。因此，Phoenix Adapter 应定位为 SQL 查询适配器，而不能假定它完全支持原生 HBase 的 Cell、时间戳和版本语义。

### 3.2 两级抽象设计

建议采用两级接口：

- Extension 侧：`IRemoteScanProvider`
- Gateway 侧：`GatewayAdapter`

两者通过统一协议通信。

```
DuckDB-HBase Extension
          |
          v
IRemoteScanProvider
          |
          v
Gateway Protocol
          |
          v
Gateway Server
          |
          v
GatewayAdapter SPI
          |
     ┌────┼────┬─────┐
     v    v    v     v
   Java REST Thrift Phoenix
     |    |    |     |
     └────┴────┴─────┘
          |
          v
        HBase
```

这样设计有两个好处：

1. Extension 无须了解底层接入协议。
2. Gateway 无须了解 DuckDB 的内部执行模型。

### 3.3 Extension 侧 Provider 接口

建议统一定义：

```
class IRemoteScanProvider {
public:
    virtual ProviderCapabilities GetCapabilities() = 0;

    virtual unique_ptr<IScanStream> Scan(
        const ScanRequest &request
    ) = 0;

    virtual unique_ptr<IGetStream> BatchGet(
        const BatchGetRequest &request
    ) = 0;

    virtual TableSchema GetSchema(
        const TableIdentifier &table
    ) = 0;

    virtual void Cancel(
        const string &request_id
    ) = 0;

    virtual ProviderHealth CheckHealth() = 0;

    virtual ~IRemoteScanProvider() = default;
};
```

这里不建议让 `Scan()` 直接返回 `vector<DataChunk>`。

因为大数据量 Scan 必须支持流式处理，不能把全部 HBase 结果一次性加载到内存。

定义两个独立的流接口：

```
class IScanStream {
public:
    virtual bool Next(ArrowRecordBatch &batch) = 0;
    virtual void Cancel() = 0;
    virtual ~IScanStream() = default;
};

class IGetStream {
public:
    virtual bool Next(ArrowRecordBatch &batch) = 0;
    virtual void Cancel() = 0;
    virtual ~IGetStream() = default;
};
```

这样 Cache Manager 可以一边接收远端数据，一边写入本地 Parquet，不需要完整数据集驻留内存。

### 3.4 Provider Capabilities

不同接入方式的能力不同，不能假定所有 Gateway 都支持同样的功能。

例如：

- Native Java Client 支持原生 Scan 和 Batch Get。
- REST 是否支持批量 Get，需要根据具体 REST Server 版本确认。
- Phoenix JDBC 不一定能返回原生 HBase Cell Timestamp。
- 部分自定义 Gateway 可能只支持 Scan，不支持 Get。

因此必须进行能力协商。

```
struct ProviderCapabilities {
    bool supports_scan;
    bool supports_batch_get;
    bool supports_rowkey_range;
    bool supports_column_projection;
    bool supports_timestamp;
    bool supports_versions;
    bool supports_filter_pushdown;
    bool supports_async_scan;
    bool supports_cancellation;

    int max_batch_size;
    int max_parallel_scans;
};
```

Extension 在连接初始化时获取能力。

如果 Gateway 不支持 Batch Get，Partial Hit 可以退化为范围 Scan。

如果不支持时间戳查询，则不能将其当作支持特定 HBase 时间戳语义的 Provider 使用。

### 3.5 Gateway 协议定义

推荐使用 Protobuf 定义统一协议。

协议不应该暴露 Java HBase Client 的内部对象，而应该表达业务级 Scan 语义。

核心协议对象：

```
syntax = "proto3";

package duckdb.hbase.v1;

service HBaseDataService {
    rpc GetCapabilities(CapabilityRequest)
        returns (CapabilityResponse);

    rpc GetSchema(SchemaRequest)
        returns (SchemaResponse);

    rpc Scan(ScanRequest)
        returns (stream ArrowBatch);

    rpc BatchGet(BatchGetRequest)
        returns (stream ArrowBatch);

    rpc Cancel(CancelRequest)
        returns (CancelResponse);

    rpc Health(HealthRequest)
        returns (HealthResponse);
}

message ColumnRef {
    string family = 1;
    string qualifier = 2;
}

message ScanRequest {
    string request_id = 1;
    string namespace = 2;
    string table = 3;

    bytes start_row = 4;
    bytes stop_row = 5;

    bool include_start = 6;
    bool include_stop = 7;

    repeated ColumnRef columns = 8;

    int64 min_timestamp = 9;
    int64 max_timestamp = 10;

    int32 max_versions = 11;
    int32 batch_size = 12;
}

message BatchGetRequest {
    string request_id = 1;
    string namespace = 2;
    string table = 3;

    repeated bytes rowkeys = 4;
    repeated ColumnRef columns = 5;

    int64 min_timestamp = 6;
    int64 max_timestamp = 7;
}

message ArrowBatch {
    bytes ipc_payload = 1;
    uint64 sequence_number = 2;
    bool finished = 3;
    uint64 row_count = 4;
}
```

生产实现还需要增加：

- Schema 和 Logical Type 描述。
- 错误码和错误详情。
- Request Deadline。
- Trace ID。
- 分页及续传能力（如需要）。
- TLS、身份认证。
- 服务端限流和资源限制。

特别注意：RowKey 必须作为 Binary 数据处理，不能默认 UTF-8。HBase 的 RowKey 是字节数组，直接转换为字符串可能破坏排序和范围比较语义。

### 3.6 Gateway SPI 设计

Java Gateway 内部再进行一次抽象。

```
public interface HBaseAccessAdapter {

    AdapterCapabilities capabilities();

    TableSchema getSchema(TableIdentifier table);

    ScanCursor scan(ScanRequest request);

    GetCursor batchGet(BatchGetRequest request);

    void cancel(String requestId);

    HealthStatus health();
}
```

这里的 `ScanCursor` 和 `GetCursor` 是 Gateway 内部接口，不直接暴露 HBase 的 `ResultScanner`。

#### Native Java Adapter

```
public class NativeHBaseAdapter
        implements HBaseAccessAdapter {

    private Connection connection;

    @Override
    public ScanCursor scan(ScanRequest request) {
        // Convert request to HBase Scan
        // Create ResultScanner
        // Return streaming cursor
    }

    @Override
    public GetCursor batchGet(BatchGetRequest request) {
        // Convert rowkeys to Get
        // Execute Table.get(List<Get>)
        // Return cursor
    }
}
```

底层依赖：

```
HBaseAccessAdapter
        |
        v
NativeHBaseAdapter
        |
        v
HBase Connection
        |
        +-- Table
        +-- Scan
        +-- Get
        +-- ResultScanner
```

#### REST Adapter

```
public class RestHBaseAdapter
        implements HBaseAccessAdapter {

    private RestClient client;

    @Override
    public ScanCursor scan(ScanRequest request) {
        // Translate to REST scan
    }

    @Override
    public GetCursor batchGet(BatchGetRequest request) {
        // Batch requests if supported
        // Otherwise report capability limitation
    }
}
```

#### Thrift Adapter

```
public class ThriftHBaseAdapter
        implements HBaseAccessAdapter {

    private HBaseThriftClient client;

    // Scan / Get translation
}
```

#### Phoenix Adapter

```
public class PhoenixAdapter
        implements HBaseAccessAdapter {

    private DataSource dataSource;

    // Translate supported requests into
    // parameterized Phoenix SQL.
}
```

Phoenix Adapter 应限制支持的 Scan 语义，并在能力声明中明确不支持的部分。

### 3.7 Gateway Factory 与配置化装配

Gateway 不应该通过修改代码选择，而应该通过配置选择。

建议配置：

```
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

Gateway Factory：

```
public class AdapterFactory {

    public static HBaseAccessAdapter create(
            GatewayConfig config) {

        switch (config.adapter()) {
            case NATIVE_JAVA:
                return new NativeHBaseAdapter(config);

            case REST:
                return new RestHBaseAdapter(config);

            case THRIFT:
                return new ThriftHBaseAdapter(config);

            case PHOENIX:
                return new PhoenixAdapter(config);

            default:
                throw new UnsupportedOperationException();
        }
    }
}
```

建议增加 Adapter SPI 的版本号，避免不同 Gateway 实现与 Extension 协议之间产生不兼容。

### 3.8 Gateway 替换时的依赖变化

\#chatgpt-mermaid-\_r_1bc\_{font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:16px;fill:rgb(13, 13, 13);}@keyframes edge-animation-frame{from{stroke-dashoffset:0;}}@keyframes dash{to{stroke-dashoffset:0;}}#chatgpt-mermaid-\_r_1bc\_ .edge-animation-slow{stroke-dasharray:9,5!important;stroke-dashoffset:900;animation:dash 50s linear infinite;stroke-linecap:round;}#chatgpt-mermaid-\_r_1bc\_ .edge-animation-fast{stroke-dasharray:9,5!important;stroke-dashoffset:900;animation:dash 20s linear infinite;stroke-linecap:round;}#chatgpt-mermaid-\_r_1bc\_ .error-icon{fill:rgb(243, 243, 243);}#chatgpt-mermaid-\_r_1bc\_ .error-text{fill:rgb(13, 13, 13);stroke:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1bc\_ .edge-thickness-normal{stroke-width:1px;}#chatgpt-mermaid-\_r_1bc\_ .edge-thickness-thick{stroke-width:3.5px;}#chatgpt-mermaid-\_r_1bc\_ .edge-pattern-solid{stroke-dasharray:0;}#chatgpt-mermaid-\_r_1bc\_ .edge-thickness-invisible{stroke-width:0;fill:none;}#chatgpt-mermaid-\_r_1bc\_ .edge-pattern-dashed{stroke-dasharray:3;}#chatgpt-mermaid-\_r_1bc\_ .edge-pattern-dotted{stroke-dasharray:2;}#chatgpt-mermaid-\_r_1bc\_ .marker{fill:rgb(143, 143, 143);stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_1bc\_ .marker.cross{stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_1bc\_ svg{font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:16px;}#chatgpt-mermaid-\_r_1bc\_ p{margin:0;}#chatgpt-mermaid-\_r_1bc\_ .label{font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1bc\_ .cluster-label text{fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1bc\_ .cluster-label span{color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1bc\_ .cluster-label span p{background-color:transparent;}#chatgpt-mermaid-\_r_1bc\_ .label text,#chatgpt-mermaid-\_r_1bc\_ span{fill:rgb(13, 13, 13);color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1bc\_ .node rect,#chatgpt-mermaid-\_r_1bc\_ .node circle,#chatgpt-mermaid-\_r_1bc\_ .node ellipse,#chatgpt-mermaid-\_r_1bc\_ .node polygon,#chatgpt-mermaid-\_r_1bc\_ .node path{fill:rgb(222, 234, 251);stroke:rgb(83, 154, 248);stroke-width:1px;}#chatgpt-mermaid-\_r_1bc\_ .rough-node .label text,#chatgpt-mermaid-\_r_1bc\_ .node .label text,#chatgpt-mermaid-\_r_1bc\_ .image-shape .label,#chatgpt-mermaid-\_r_1bc\_ .icon-shape .label{text-anchor:middle;}#chatgpt-mermaid-\_r_1bc\_ .node .katex path{fill:#000;stroke:#000;stroke-width:1px;}#chatgpt-mermaid-\_r_1bc\_ .rough-node .label,#chatgpt-mermaid-\_r_1bc\_ .node .label,#chatgpt-mermaid-\_r_1bc\_ .image-shape .label,#chatgpt-mermaid-\_r_1bc\_ .icon-shape .label{text-align:center;}#chatgpt-mermaid-\_r_1bc\_ .node.clickable{cursor:pointer;}#chatgpt-mermaid-\_r_1bc\_ .root .anchor path{fill:rgb(143, 143, 143)!important;stroke-width:0;stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_1bc\_ .arrowheadPath{fill:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_1bc\_ .edgePath .path{stroke:rgb(143, 143, 143);stroke-width:1px;}#chatgpt-mermaid-\_r_1bc\_ .flowchart-link{stroke:rgb(143, 143, 143);fill:none;}#chatgpt-mermaid-\_r_1bc\_ .edgeLabel{background-color:rgb(252, 252, 252);text-align:center;}#chatgpt-mermaid-\_r_1bc\_ .edgeLabel p{background-color:rgb(252, 252, 252);}#chatgpt-mermaid-\_r_1bc\_ .edgeLabel rect{opacity:0.5;background-color:rgb(252, 252, 252);fill:rgb(252, 252, 252);}#chatgpt-mermaid-\_r_1bc\_ .labelBkg{background-color:rgba(252, 252, 252, 0.5);}#chatgpt-mermaid-\_r_1bc\_ .cluster rect{fill:rgb(243, 243, 243);stroke:rgba(0, 0, 0, 0.1);stroke-width:1px;}#chatgpt-mermaid-\_r_1bc\_ .cluster text{fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1bc\_ .cluster span{color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1bc\_ div.mermaidTooltip{position:absolute;text-align:center;max-width:200px;padding:2px;font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:12px;background:rgb(243, 243, 243);border:1px solid rgba(0, 0, 0, 0.1);border-radius:2px;pointer-events:none;z-index:100;}#chatgpt-mermaid-\_r_1bc\_ .flowchartTitleText{text-anchor:middle;font-size:18px;fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1bc\_ rect.text{fill:none;stroke-width:0;}#chatgpt-mermaid-\_r_1bc\_ .icon-shape,#chatgpt-mermaid-\_r_1bc\_ .image-shape{background-color:rgb(252, 252, 252);text-align:center;}#chatgpt-mermaid-\_r_1bc\_ .icon-shape p,#chatgpt-mermaid-\_r_1bc\_ .image-shape p{background-color:rgb(252, 252, 252);padding:2px;}#chatgpt-mermaid-\_r_1bc\_ .icon-shape .label rect,#chatgpt-mermaid-\_r_1bc\_ .image-shape .label rect{opacity:0.5;background-color:rgb(252, 252, 252);fill:rgb(252, 252, 252);}#chatgpt-mermaid-\_r_1bc\_ .label-icon{display:inline-block;height:1em;overflow:visible;vertical-align:-0.125em;}#chatgpt-mermaid-\_r_1bc\_ .node .label-icon path{fill:currentColor;stroke:revert;stroke-width:revert;}#chatgpt-mermaid-\_r_1bc\_ .node .neo-node{stroke:rgb(83, 154, 248);}#chatgpt-mermaid-\_r_1bc\_ [data-look="neo"].node rect,#chatgpt-mermaid-\_r_1bc\_ [data-look="neo"].cluster rect,#chatgpt-mermaid-\_r_1bc\_ [data-look="neo"].node polygon{stroke:url(#chatgpt-mermaid-\_r_1bc\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1bc\_ [data-look="neo"].swimlane.cluster rect{filter:none;}#chatgpt-mermaid-\_r_1bc\_ [data-look="neo"].node path{stroke:url(#chatgpt-mermaid-\_r_1bc\_-gradient);stroke-width:1px;}#chatgpt-mermaid-\_r_1bc\_ [data-look="neo"].node .outer-path{filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1bc\_ [data-look="neo"].node .neo-line path{stroke:rgb(83, 154, 248);filter:none;}#chatgpt-mermaid-\_r_1bc\_ [data-look="neo"].node circle{stroke:url(#chatgpt-mermaid-\_r_1bc\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1bc\_ [data-look="neo"].node circle .state-start{fill:#000000;}#chatgpt-mermaid-\_r_1bc\_ [data-look="neo"].icon-shape .icon{fill:url(#chatgpt-mermaid-\_r_1bc\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1bc\_ [data-look="neo"].icon-shape .icon-neo path{stroke:url(#chatgpt-mermaid-\_r_1bc\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1bc\_ .node text{font-size:14px;font-weight:600;letter-spacing:normal;fill:rgb(0, 79, 153);}#chatgpt-mermaid-\_r_1bc\_ .edgeLabels text{font-size:13px;font-weight:600;letter-spacing:-0.08px;fill:rgb(0, 79, 153);}#chatgpt-mermaid-\_r_1bc\_ .node tspan[font-weight="normal"],#chatgpt-mermaid-\_r_1bc\_ .edgeLabels tspan[font-weight="normal"]{font-weight:600;}#chatgpt-mermaid-\_r_1bc\_ .edgeLabel .label rect{opacity:1;rx:13px;ry:13px;fill:rgb(245, 250, 255);stroke:rgb(206, 219, 229);stroke-width:1px;}#chatgpt-mermaid-\_r_1bc\_ .node rect,#chatgpt-mermaid-\_r_1bc\_ .node circle,#chatgpt-mermaid-\_r_1bc\_ .node ellipse,#chatgpt-mermaid-\_r_1bc\_ .node polygon,#chatgpt-mermaid-\_r_1bc\_ .node path{fill:rgb(229, 243, 255);stroke:rgba(0, 0, 0, 0.1);stroke-width:1px;}#chatgpt-mermaid-\_r_1bc\_ .node rect{rx:16px;ry:16px;}#chatgpt-mermaid-\_r_1bc\_ .mermaid-decision .label-container{fill:rgb(245, 250, 255);stroke:rgb(206, 219, 229);stroke-dasharray:2,2;}#chatgpt-mermaid-\_r_1bc\_ .edgePaths .flowchart-link{stroke:rgb(143, 143, 143);stroke-width:1px;stroke-linecap:round;stroke-linejoin:round;}#chatgpt-mermaid-\_r_1bc\_ .marker{fill:rgb(143, 143, 143);stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_1bc\_ :root{--mermaid-font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";}DuckDB ExtensionRemote ProviderGateway ProtocolNative JavaRESTThriftPhoenixHBase

切换 Native Java 到 REST 时：

- Extension 不变。
- Cache 不变。
- Scan Planner 不变。
- SQL 不变。
- 只需要更换 Gateway Adapter 和配置。

这就是本次设计希望实现的核心解耦效果。

## 四、用户侧 API 设计

用户侧需要做到尽量简单。

用户应该只关心：

1. 安装 Extension。
2. 配置连接。
3. 执行 SQL。
4. 查看缓存效果。

不应该要求用户手动创建 Scan Request 或管理 Arrow 数据流。

### 4.1 SQL API 设计

建议第一版提供以下 API。

| API                   | 类型              | 作用           |
| --------------------- | --------------- | ------------ |
| `hbase_connect()`     | Scalar Function | 注册连接         |
| `hbase_disconnect()`  | Scalar Function | 关闭连接         |
| `hbase_scan()`        | Table Function  | 执行 Scan      |
| `hbase_get()`         | Table Function  | 根据 RowKey 查询 |
| `hbase_tables()`      | Table Function  | 查看表          |
| `hbase_columns()`     | Table Function  | 查看列          |
| `hbase_cache_stats()` | Table Function  | 查看缓存状态       |
| `hbase_cache_clear()` | Table Function  | 清理缓存         |

建议将连接和查询分离。

#### 注册连接

```
SELECT hbase_connect(
    'prod',
    'grpc://127.0.0.1:18080'
);
```

第二个参数也可以支持 JSON 配置：

```
SELECT hbase_connect(
    'prod',
    '{
        "endpoint": "grpc://127.0.0.1:18080",
        "timeout_ms": 30000,
        "cache_enabled": true,
        "cache_ttl": 300
    }'
);
```

生产环境不建议将密码写入 SQL 明文，可以通过环境变量、Kerberos 或受保护的配置文件获取凭据。

#### 执行 Scan

建议统一函数签名：

```
hbase_scan(
    connection_name,
    namespace,
    table_name,
    start_row := NULL,
    stop_row := NULL,
    columns := NULL,
    max_versions := 1,
    timestamp_min := NULL,
    timestamp_max := NULL,
    cache := true
)
```

示例：

```
SELECT *
FROM hbase_scan(
    'prod',
    'default',
    'customer_profile'
);
```

指定 RowKey：

```
SELECT name, age
FROM hbase_scan(
    'prod',
    'default',
    'customer_profile',
    start_row := '1000',
    stop_row := '2000',
    columns := ['cf1:name', 'cf1:age']
)
WHERE age > 30;
```

需要说明：这里的 `start_row` 和 `stop_row` 在第一版 API 中采用左闭右开语义。对二进制 RowKey，另外提供 BLOB 输入形式，不能将其一律转换成 UTF-8 字符串。

#### Get API

```
SELECT *
FROM hbase_get(
    'prod',
    'default',
    'customer_profile',
    rowkeys := ['1001', '1002', '1003']
);
```

#### 查看表结构

```
SELECT *
FROM hbase_columns(
    'prod',
    'default',
    'customer_profile'
);
```

### 4.2 Cache 管理 API

查看缓存：

```
SELECT *
FROM hbase_cache_stats();
```

清理某张表：

```
SELECT hbase_cache_clear(
    'prod',
    'default',
    'customer_profile'
);
```

清理所有缓存：

```
SELECT hbase_cache_clear();
```

建议提供配置：

```
SET hbase_cache_enabled = true;

SET hbase_cache_ttl = 300;

SET hbase_cache_max_size = '10GB';

SET hbase_cache_directory = './hbase_cache';

SET hbase_cache_mode = 'parquet';
```

`cache` 参数控制当前查询是否使用缓存，`hbase_cache_enabled` 则是全局开关。

### 4.3 用户使用流程

\#chatgpt-mermaid-\_r_1dl\_{font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:16px;fill:rgb(13, 13, 13);}@keyframes edge-animation-frame{from{stroke-dashoffset:0;}}@keyframes dash{to{stroke-dashoffset:0;}}#chatgpt-mermaid-\_r_1dl\_ .edge-animation-slow{stroke-dasharray:9,5!important;stroke-dashoffset:900;animation:dash 50s linear infinite;stroke-linecap:round;}#chatgpt-mermaid-\_r_1dl\_ .edge-animation-fast{stroke-dasharray:9,5!important;stroke-dashoffset:900;animation:dash 20s linear infinite;stroke-linecap:round;}#chatgpt-mermaid-\_r_1dl\_ .error-icon{fill:rgb(243, 243, 243);}#chatgpt-mermaid-\_r_1dl\_ .error-text{fill:rgb(13, 13, 13);stroke:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1dl\_ .edge-thickness-normal{stroke-width:1px;}#chatgpt-mermaid-\_r_1dl\_ .edge-thickness-thick{stroke-width:3.5px;}#chatgpt-mermaid-\_r_1dl\_ .edge-pattern-solid{stroke-dasharray:0;}#chatgpt-mermaid-\_r_1dl\_ .edge-thickness-invisible{stroke-width:0;fill:none;}#chatgpt-mermaid-\_r_1dl\_ .edge-pattern-dashed{stroke-dasharray:3;}#chatgpt-mermaid-\_r_1dl\_ .edge-pattern-dotted{stroke-dasharray:2;}#chatgpt-mermaid-\_r_1dl\_ .marker{fill:rgb(143, 143, 143);stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_1dl\_ .marker.cross{stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_1dl\_ svg{font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:16px;}#chatgpt-mermaid-\_r_1dl\_ p{margin:0;}#chatgpt-mermaid-\_r_1dl\_ .label{font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1dl\_ .cluster-label text{fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1dl\_ .cluster-label span{color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1dl\_ .cluster-label span p{background-color:transparent;}#chatgpt-mermaid-\_r_1dl\_ .label text,#chatgpt-mermaid-\_r_1dl\_ span{fill:rgb(13, 13, 13);color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1dl\_ .node rect,#chatgpt-mermaid-\_r_1dl\_ .node circle,#chatgpt-mermaid-\_r_1dl\_ .node ellipse,#chatgpt-mermaid-\_r_1dl\_ .node polygon,#chatgpt-mermaid-\_r_1dl\_ .node path{fill:rgb(222, 234, 251);stroke:rgb(83, 154, 248);stroke-width:1px;}#chatgpt-mermaid-\_r_1dl\_ .rough-node .label text,#chatgpt-mermaid-\_r_1dl\_ .node .label text,#chatgpt-mermaid-\_r_1dl\_ .image-shape .label,#chatgpt-mermaid-\_r_1dl\_ .icon-shape .label{text-anchor:middle;}#chatgpt-mermaid-\_r_1dl\_ .node .katex path{fill:#000;stroke:#000;stroke-width:1px;}#chatgpt-mermaid-\_r_1dl\_ .rough-node .label,#chatgpt-mermaid-\_r_1dl\_ .node .label,#chatgpt-mermaid-\_r_1dl\_ .image-shape .label,#chatgpt-mermaid-\_r_1dl\_ .icon-shape .label{text-align:center;}#chatgpt-mermaid-\_r_1dl\_ .node.clickable{cursor:pointer;}#chatgpt-mermaid-\_r_1dl\_ .root .anchor path{fill:rgb(143, 143, 143)!important;stroke-width:0;stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_1dl\_ .arrowheadPath{fill:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_1dl\_ .edgePath .path{stroke:rgb(143, 143, 143);stroke-width:1px;}#chatgpt-mermaid-\_r_1dl\_ .flowchart-link{stroke:rgb(143, 143, 143);fill:none;}#chatgpt-mermaid-\_r_1dl\_ .edgeLabel{background-color:rgb(252, 252, 252);text-align:center;}#chatgpt-mermaid-\_r_1dl\_ .edgeLabel p{background-color:rgb(252, 252, 252);}#chatgpt-mermaid-\_r_1dl\_ .edgeLabel rect{opacity:0.5;background-color:rgb(252, 252, 252);fill:rgb(252, 252, 252);}#chatgpt-mermaid-\_r_1dl\_ .labelBkg{background-color:rgba(252, 252, 252, 0.5);}#chatgpt-mermaid-\_r_1dl\_ .cluster rect{fill:rgb(243, 243, 243);stroke:rgba(0, 0, 0, 0.1);stroke-width:1px;}#chatgpt-mermaid-\_r_1dl\_ .cluster text{fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1dl\_ .cluster span{color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1dl\_ div.mermaidTooltip{position:absolute;text-align:center;max-width:200px;padding:2px;font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:12px;background:rgb(243, 243, 243);border:1px solid rgba(0, 0, 0, 0.1);border-radius:2px;pointer-events:none;z-index:100;}#chatgpt-mermaid-\_r_1dl\_ .flowchartTitleText{text-anchor:middle;font-size:18px;fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1dl\_ rect.text{fill:none;stroke-width:0;}#chatgpt-mermaid-\_r_1dl\_ .icon-shape,#chatgpt-mermaid-\_r_1dl\_ .image-shape{background-color:rgb(252, 252, 252);text-align:center;}#chatgpt-mermaid-\_r_1dl\_ .icon-shape p,#chatgpt-mermaid-\_r_1dl\_ .image-shape p{background-color:rgb(252, 252, 252);padding:2px;}#chatgpt-mermaid-\_r_1dl\_ .icon-shape .label rect,#chatgpt-mermaid-\_r_1dl\_ .image-shape .label rect{opacity:0.5;background-color:rgb(252, 252, 252);fill:rgb(252, 252, 252);}#chatgpt-mermaid-\_r_1dl\_ .label-icon{display:inline-block;height:1em;overflow:visible;vertical-align:-0.125em;}#chatgpt-mermaid-\_r_1dl\_ .node .label-icon path{fill:currentColor;stroke:revert;stroke-width:revert;}#chatgpt-mermaid-\_r_1dl\_ .node .neo-node{stroke:rgb(83, 154, 248);}#chatgpt-mermaid-\_r_1dl\_ [data-look="neo"].node rect,#chatgpt-mermaid-\_r_1dl\_ [data-look="neo"].cluster rect,#chatgpt-mermaid-\_r_1dl\_ [data-look="neo"].node polygon{stroke:url(#chatgpt-mermaid-\_r_1dl\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1dl\_ [data-look="neo"].swimlane.cluster rect{filter:none;}#chatgpt-mermaid-\_r_1dl\_ [data-look="neo"].node path{stroke:url(#chatgpt-mermaid-\_r_1dl\_-gradient);stroke-width:1px;}#chatgpt-mermaid-\_r_1dl\_ [data-look="neo"].node .outer-path{filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1dl\_ [data-look="neo"].node .neo-line path{stroke:rgb(83, 154, 248);filter:none;}#chatgpt-mermaid-\_r_1dl\_ [data-look="neo"].node circle{stroke:url(#chatgpt-mermaid-\_r_1dl\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1dl\_ [data-look="neo"].node circle .state-start{fill:#000000;}#chatgpt-mermaid-\_r_1dl\_ [data-look="neo"].icon-shape .icon{fill:url(#chatgpt-mermaid-\_r_1dl\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1dl\_ [data-look="neo"].icon-shape .icon-neo path{stroke:url(#chatgpt-mermaid-\_r_1dl\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1dl\_ .node text{font-size:14px;font-weight:600;letter-spacing:normal;fill:rgb(0, 79, 153);}#chatgpt-mermaid-\_r_1dl\_ .edgeLabels text{font-size:13px;font-weight:600;letter-spacing:-0.08px;fill:rgb(0, 79, 153);}#chatgpt-mermaid-\_r_1dl\_ .node tspan[font-weight="normal"],#chatgpt-mermaid-\_r_1dl\_ .edgeLabels tspan[font-weight="normal"]{font-weight:600;}#chatgpt-mermaid-\_r_1dl\_ .edgeLabel .label rect{opacity:1;rx:13px;ry:13px;fill:rgb(245, 250, 255);stroke:rgb(206, 219, 229);stroke-width:1px;}#chatgpt-mermaid-\_r_1dl\_ .node rect,#chatgpt-mermaid-\_r_1dl\_ .node circle,#chatgpt-mermaid-\_r_1dl\_ .node ellipse,#chatgpt-mermaid-\_r_1dl\_ .node polygon,#chatgpt-mermaid-\_r_1dl\_ .node path{fill:rgb(229, 243, 255);stroke:rgba(0, 0, 0, 0.1);stroke-width:1px;}#chatgpt-mermaid-\_r_1dl\_ .node rect{rx:16px;ry:16px;}#chatgpt-mermaid-\_r_1dl\_ .mermaid-decision .label-container{fill:rgb(245, 250, 255);stroke:rgb(206, 219, 229);stroke-dasharray:2,2;}#chatgpt-mermaid-\_r_1dl\_ .edgePaths .flowchart-link{stroke:rgb(143, 143, 143);stroke-width:1px;stroke-linecap:round;stroke-linejoin:round;}#chatgpt-mermaid-\_r_1dl\_ .marker{fill:rgb(143, 143, 143);stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_1dl\_ :root{--mermaid-font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";}安装 DuckDB安装 duckdb_hbase启动 Java GatewayLOAD hbasehbase_connect执行 hbase_scan SQLCache Hit?本地 CacheGateway ScanHBaseCache WriteDuckDB SQL ExecutionQuery Result是否

## 五、安装、部署与使用说明

需要区分三个独立的安装对象：

- DuckDB。
- DuckDB-HBase Extension。
- Java Gateway。

Extension 不负责自动安装和启动 Java Gateway。

### 5.1 安装 DuckDB CLI

以 Linux 为例：

```
curl -LO https://github.com/duckdb/duckdb/releases/latest/download/duckdb_cli-linux-amd64.zip

unzip duckdb_cli-linux-amd64.zip

./duckdb
```

实际生产部署应固定 DuckDB 版本，而不是始终使用 latest。

### 5.2 安装 Python DuckDB

```
pip install duckdb
```

Python：

```
import duckdbcon = duckdb.connect("analytics.duckdb")print(con.execute("SELECT version()").fetchall())
```

### 5.3 安装 DuckDB-HBase Extension

第一阶段建议提供两种安装方式。

方式 A：本地安装

开发阶段最简单。

假设编译产物：

```
build/release/extension/hbase/hbase.duckdb_extension
```

用户执行：

```
LOAD '/opt/duckdb/extensions/hbase.duckdb_extension';
```

具体动态库文件名以构建产物为准。

如果已经通过 Extension Repository 发布，则可以：

```
INSTALL hbase FROM community;
LOAD hbase;
```

这条命令是未来发布到 Community Repository 后的使用形式，不代表当前已经发布。

方式 B：Python 加载

```
import duckdbcon = duckdb.connect("analytics.duckdb")con.execute("""    LOAD '/opt/duckdb/extensions/hbase.duckdb_extension'""")
```

### 5.4 启动 Gateway

建议 Gateway 提供独立发行包：

```
duckdb-hbase-gateway/
    bin/
        start.sh
        stop.sh
    conf/
        application.yml
    lib/
        gateway.jar
        hbase-client.jar
```

启动：

```
java -jar gateway.jar \
    --spring.config.location=conf/application.yml
```

建议提供健康检查：

```
curl http://127.0.0.1:18080/health
```

生产环境可以通过 systemd、Docker 或 Kubernetes 管理 Gateway 生命周期。

Gateway 必须部署在能够访问 HBase 集群的网络环境中。

### 5.5 完整用户操作示例

DuckDB CLI：

```
-- 1. 加载扩展
LOAD '/opt/duckdb/extensions/hbase.duckdb_extension';

-- 2. 注册连接
SELECT hbase_connect(
    'prod',
    'grpc://127.0.0.1:18080'
);

-- 3. 首次查询
SELECT name, age, city
FROM hbase_scan(
    'prod',
    'default',
    'customer_profile',
    start_row := '1000',
    stop_row := '2000'
)
WHERE age > 30;

-- 4. 第二次查询
-- 使用相同 RowKey 范围，但过滤条件不同
SELECT name, city
FROM hbase_scan(
    'prod',
    'default',
    'customer_profile',
    start_row := '1000',
    stop_row := '2000'
)
WHERE age > 40;

-- 5. 查看缓存命中情况
SELECT *
FROM hbase_cache_stats();
```

Python：

```
import duckdbcon = duckdb.connect("analytics.duckdb")con.execute("""    LOAD '/opt/duckdb/extensions/hbase.duckdb_extension'""")con.execute("""    SELECT hbase_connect(        'prod',        'grpc://127.0.0.1:18080'    )""")result = con.execute("""    SELECT name, age, city    FROM hbase_scan(        'prod',        'default',        'customer_profile',        start_row := '1000',        stop_row := '2000'    )    WHERE age > 30""").fetchdf()print(result)
```

## 六、详细分包设计与项目工程结构

建议采用 Monorepo，但 C++ Extension 和 Java Gateway 使用各自独立的构建系统。

```
duckdb-hbase/
│
├── extension/                         # C++ DuckDB Extension
│   ├── src/
│   │   ├── extension/
│   │   │   ├── hbase_extension.cpp
│   │   │   ├── hbase_extension_state.cpp
│   │   │   └── function_registry.cpp
│   │   │
│   │   ├── sql/
│   │   │   ├── hbase_scan_function.cpp
│   │   │   ├── hbase_get_function.cpp
│   │   │   ├── hbase_connect_function.cpp
│   │   │   ├── hbase_cache_functions.cpp
│   │   │   └── hbase_metadata_functions.cpp
│   │   │
│   │   ├── scan/
│   │   │   ├── scan_planner.cpp
│   │   │   ├── scan_executor.cpp
│   │   │   ├── scan_pipeline.cpp
│   │   │   ├── projection_planner.cpp
│   │   │   ├── predicate_planner.cpp
│   │   │   ├── rowkey_range_planner.cpp
│   │   │   └── data_chunk_converter.cpp
│   │   │
│   │   ├── cache/
│   │   │   ├── cache_manager.cpp
│   │   │   ├── cache_lookup_engine.cpp
│   │   │   ├── cache_coverage_index.cpp
│   │   │   ├── cache_segment_manager.cpp
│   │   │   ├── cache_merger.cpp
│   │   │   ├── cache_invalidation.cpp
│   │   │   └── cache_eviction.cpp
│   │   │
│   │   ├── storage/
│   │   │   ├── parquet_cache_storage.cpp
│   │   │   ├── memory_cache_storage.cpp
│   │   │   ├── metadata_store.cpp
│   │   │   └── segment_manifest.cpp
│   │   │
│   │   ├── provider/
│   │   │   ├── remote_scan_provider.hpp
│   │   │   ├── grpc_provider.cpp
│   │   │   ├── http_provider.cpp
│   │   │   ├── provider_factory.cpp
│   │   │   └── arrow_stream_reader.cpp
│   │   │
│   │   ├── catalog/
│   │   │   ├── schema_provider.cpp
│   │   │   ├── column_mapping.cpp
│   │   │   └── type_mapping.cpp
│   │   │
│   │   └── common/
│   │       ├── config.cpp
│   │       ├── errors.cpp
│   │       ├── logger.cpp
│   │       ├── types.cpp
│   │       └── rowkey.cpp
│   │
│   ├── include/
│   │   ├── scan/
│   │   ├── cache/
│   │   ├── storage/
│   │   ├── provider/
│   │   └── common/
│   │
│   ├── test/
│   │   ├── unit/
│   │   ├── sql/
│   │   └── integration/
│   │
│   └── CMakeLists.txt
│
├── gateway/                           # Java Gateway
│   ├── gateway-api/
│   │   └── src/main/java/
│   │       └── protocol/
│   │
│   ├── gateway-core/
│   │   └── src/main/java/
│   │       ├── spi/
│   │       ├── service/
│   │       ├── factory/
│   │       ├── model/
│   │       ├── security/
│   │       └── streaming/
│   │
│   ├── adapter-native/
│   ├── adapter-rest/
│   ├── adapter-thrift/
│   ├── adapter-phoenix/
│   ├── gateway-server/
│   └── pom.xml
│
├── protocol/
│   └── proto/
│       └── hbase_data_service.proto
│
├── scripts/
│   ├── build-extension.sh
│   ├── build-gateway.sh
│   ├── package.sh
│   └── integration-test.sh
│
├── examples/
│   ├── sql/
│   ├── python/
│   └── config/
│
├── docs/
│   ├── architecture.md
│   ├── extension-design.md
│   ├── gateway-design.md
│   ├── cache-design.md
│   ├── user-guide.md
│   └── deployment-guide.md
│
└── README.md
```

### 6.1 C++ 命名空间设计

建议按领域命名，避免类名冲突：

```
namespace duckdb {
namespace hbase {

namespace sql {}
namespace scan {}
namespace cache {}
namespace storage {}
namespace provider {}
namespace catalog {}
namespace common {}

}
}
```

接口头文件与具体实现分离。

例如：

```
include/provider/
    remote_scan_provider.hpp
    provider_capabilities.hpp
    provider_session.hpp

src/provider/
    grpc_provider.cpp
    http_provider.cpp
    provider_factory.cpp
```

`scan` 不能直接 include `grpc_provider.hpp`。

它只应该依赖 `remote_scan_provider.hpp`。

### 6.2 Java Maven 模块设计

建议依赖方向：

```
gateway-server
      |
      v
gateway-core
      |
      v
gateway-api

adapter-native ----> gateway-api
adapter-rest ------> gateway-api
adapter-thrift ----> gateway-api
adapter-phoenix ---> gateway-api
```

具体依赖：

| 模块              | 依赖                           |
| --------------- | ---------------------------- |
| gateway-api     | Protobuf、统一 DTO              |
| gateway-core    | gateway-api、SPI              |
| adapter-native  | gateway-api、HBase Client 2.x |
| adapter-rest    | gateway-api、HTTP Client      |
| adapter-thrift  | gateway-api、Thrift Client    |
| adapter-phoenix | gateway-api、Phoenix JDBC     |
| gateway-server  | gateway-core、gRPC、Arrow      |

Gateway Server 不应该直接引用 `NativeHBaseAdapter`，而是由 Factory 装配 `HBaseAccessAdapter`。

### 6.3 主要类之间的关系

\#chatgpt-mermaid-\_r_1h0\_{font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:16px;fill:rgb(13, 13, 13);}@keyframes edge-animation-frame{from{stroke-dashoffset:0;}}@keyframes dash{to{stroke-dashoffset:0;}}#chatgpt-mermaid-\_r_1h0\_ .edge-animation-slow{stroke-dasharray:9,5!important;stroke-dashoffset:900;animation:dash 50s linear infinite;stroke-linecap:round;}#chatgpt-mermaid-\_r_1h0\_ .edge-animation-fast{stroke-dasharray:9,5!important;stroke-dashoffset:900;animation:dash 20s linear infinite;stroke-linecap:round;}#chatgpt-mermaid-\_r_1h0\_ .error-icon{fill:rgb(243, 243, 243);}#chatgpt-mermaid-\_r_1h0\_ .error-text{fill:rgb(13, 13, 13);stroke:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1h0\_ .edge-thickness-normal{stroke-width:1px;}#chatgpt-mermaid-\_r_1h0\_ .edge-thickness-thick{stroke-width:3.5px;}#chatgpt-mermaid-\_r_1h0\_ .edge-pattern-solid{stroke-dasharray:0;}#chatgpt-mermaid-\_r_1h0\_ .edge-thickness-invisible{stroke-width:0;fill:none;}#chatgpt-mermaid-\_r_1h0\_ .edge-pattern-dashed{stroke-dasharray:3;}#chatgpt-mermaid-\_r_1h0\_ .edge-pattern-dotted{stroke-dasharray:2;}#chatgpt-mermaid-\_r_1h0\_ .marker{fill:rgb(143, 143, 143);stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_1h0\_ .marker.cross{stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_1h0\_ svg{font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:16px;}#chatgpt-mermaid-\_r_1h0\_ p{margin:0;}#chatgpt-mermaid-\_r_1h0\_ g.classGroup text{fill:rgb(83, 154, 248);stroke:none;font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:10px;}#chatgpt-mermaid-\_r_1h0\_ g.classGroup text .title{font-weight:bolder;}#chatgpt-mermaid-\_r_1h0\_ .cluster-label text{fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1h0\_ .cluster-label span{color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1h0\_ .cluster-label span p{background-color:transparent;}#chatgpt-mermaid-\_r_1h0\_ .cluster rect{fill:rgb(243, 243, 243);stroke:rgba(0, 0, 0, 0.1);stroke-width:1px;}#chatgpt-mermaid-\_r_1h0\_ .cluster text{fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1h0\_ .cluster span{color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1h0\_ .nodeLabel,#chatgpt-mermaid-\_r_1h0\_ .edgeLabel{color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1h0\_ .noteLabel .nodeLabel,#chatgpt-mermaid-\_r_1h0\_ .noteLabel .edgeLabel{color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1h0\_ .edgeLabel .label rect{fill:rgb(222, 234, 251);}#chatgpt-mermaid-\_r_1h0\_ .label text{fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1h0\_ .labelBkg{background:rgb(222, 234, 251);}#chatgpt-mermaid-\_r_1h0\_ .edgeLabel .label span{background:rgb(222, 234, 251);}#chatgpt-mermaid-\_r_1h0\_ .classTitle{font-weight:bolder;}#chatgpt-mermaid-\_r_1h0\_ .node rect,#chatgpt-mermaid-\_r_1h0\_ .node circle,#chatgpt-mermaid-\_r_1h0\_ .node ellipse,#chatgpt-mermaid-\_r_1h0\_ .node polygon,#chatgpt-mermaid-\_r_1h0\_ .node path{fill:rgb(222, 234, 251);stroke:rgb(83, 154, 248);stroke-width:1;}#chatgpt-mermaid-\_r_1h0\_ .divider{stroke:rgb(83, 154, 248);stroke-width:1;}#chatgpt-mermaid-\_r_1h0\_ g.clickable{cursor:pointer;}#chatgpt-mermaid-\_r_1h0\_ g.classGroup rect{fill:rgb(222, 234, 251);stroke:rgb(83, 154, 248);}#chatgpt-mermaid-\_r_1h0\_ g.classGroup line{stroke:rgb(83, 154, 248);stroke-width:1;}#chatgpt-mermaid-\_r_1h0\_ .classLabel .box{stroke:none;stroke-width:0;fill:rgb(222, 234, 251);opacity:0.5;}#chatgpt-mermaid-\_r_1h0\_ .classLabel .label{fill:rgb(83, 154, 248);font-size:10px;}#chatgpt-mermaid-\_r_1h0\_ .relation{stroke:rgb(143, 143, 143);stroke-width:1;fill:none;}#chatgpt-mermaid-\_r_1h0\_ .dashed-line{stroke-dasharray:3;}#chatgpt-mermaid-\_r_1h0\_ .dotted-line{stroke-dasharray:1 2;}#chatgpt-mermaid-\_r_1h0\_ [id$="-compositionStart"],#chatgpt-mermaid-\_r_1h0\_ .composition{fill:rgb(143, 143, 143)!important;stroke:rgb(143, 143, 143)!important;stroke-width:1;}#chatgpt-mermaid-\_r_1h0\_ [id$="-compositionEnd"],#chatgpt-mermaid-\_r_1h0\_ .composition{fill:rgb(143, 143, 143)!important;stroke:rgb(143, 143, 143)!important;stroke-width:1;}#chatgpt-mermaid-\_r_1h0\_ [id$="-dependencyStart"],#chatgpt-mermaid-\_r_1h0\_ .dependency{fill:rgb(143, 143, 143)!important;stroke:rgb(143, 143, 143)!important;stroke-width:1;}#chatgpt-mermaid-\_r_1h0\_ [id$="-dependencyEnd"],#chatgpt-mermaid-\_r_1h0\_ .dependency{fill:rgb(143, 143, 143)!important;stroke:rgb(143, 143, 143)!important;stroke-width:1;}#chatgpt-mermaid-\_r_1h0\_ [id$="-extensionStart"],#chatgpt-mermaid-\_r_1h0\_ .extension{fill:transparent!important;stroke:rgb(143, 143, 143)!important;stroke-width:1;}#chatgpt-mermaid-\_r_1h0\_ [id$="-extensionEnd"],#chatgpt-mermaid-\_r_1h0\_ .extension{fill:transparent!important;stroke:rgb(143, 143, 143)!important;stroke-width:1;}#chatgpt-mermaid-\_r_1h0\_ [id$="-aggregationStart"],#chatgpt-mermaid-\_r_1h0\_ .aggregation{fill:transparent!important;stroke:rgb(143, 143, 143)!important;stroke-width:1;}#chatgpt-mermaid-\_r_1h0\_ [id$="-aggregationEnd"],#chatgpt-mermaid-\_r_1h0\_ .aggregation{fill:transparent!important;stroke:rgb(143, 143, 143)!important;stroke-width:1;}#chatgpt-mermaid-\_r_1h0\_ [id$="-lollipopStart"],#chatgpt-mermaid-\_r_1h0\_ .lollipop{fill:rgb(222, 234, 251)!important;stroke:rgb(143, 143, 143)!important;stroke-width:1;}#chatgpt-mermaid-\_r_1h0\_ [id$="-lollipopEnd"],#chatgpt-mermaid-\_r_1h0\_ .lollipop{fill:rgb(222, 234, 251)!important;stroke:rgb(143, 143, 143)!important;stroke-width:1;}#chatgpt-mermaid-\_r_1h0\_ .edgeTerminals{font-size:11px;line-height:initial;}#chatgpt-mermaid-\_r_1h0\_ .classTitleText{text-anchor:middle;font-size:18px;fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1h0\_ .edgeLabel[data-look="neo"]{background-color:rgb(252, 252, 252);text-align:center;}#chatgpt-mermaid-\_r_1h0\_ .edgeLabel[data-look="neo"] p{background-color:rgb(252, 252, 252);}#chatgpt-mermaid-\_r_1h0\_ .edgeLabel[data-look="neo"] rect{opacity:0.5;background-color:rgb(252, 252, 252);fill:rgb(252, 252, 252);}#chatgpt-mermaid-\_r_1h0\_ .label-icon{display:inline-block;height:1em;overflow:visible;vertical-align:-0.125em;}#chatgpt-mermaid-\_r_1h0\_ .node .label-icon path{fill:currentColor;stroke:revert;stroke-width:revert;}#chatgpt-mermaid-\_r_1h0\_ .node .neo-node{stroke:rgb(83, 154, 248);}#chatgpt-mermaid-\_r_1h0\_ [data-look="neo"].node rect,#chatgpt-mermaid-\_r_1h0\_ [data-look="neo"].cluster rect,#chatgpt-mermaid-\_r_1h0\_ [data-look="neo"].node polygon{stroke:url(#chatgpt-mermaid-\_r_1h0\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1h0\_ [data-look="neo"].swimlane.cluster rect{filter:none;}#chatgpt-mermaid-\_r_1h0\_ [data-look="neo"].node path{stroke:url(#chatgpt-mermaid-\_r_1h0\_-gradient);stroke-width:1px;}#chatgpt-mermaid-\_r_1h0\_ [data-look="neo"].node .outer-path{filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1h0\_ [data-look="neo"].node .neo-line path{stroke:rgb(83, 154, 248);filter:none;}#chatgpt-mermaid-\_r_1h0\_ [data-look="neo"].node circle{stroke:url(#chatgpt-mermaid-\_r_1h0\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1h0\_ [data-look="neo"].node circle .state-start{fill:#000000;}#chatgpt-mermaid-\_r_1h0\_ [data-look="neo"].icon-shape .icon{fill:url(#chatgpt-mermaid-\_r_1h0\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1h0\_ [data-look="neo"].icon-shape .icon-neo path{stroke:url(#chatgpt-mermaid-\_r_1h0\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1h0\_ :root{--mermaid-font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";}HBaseExtension+Load()+RegisterFunctions()HBaseScanFunction+Bind()+InitGlobalState()+Execute()ScanPlanner+BuildPlan()ScanExecutor+Execute()+Cancel()«interface»ICacheManager+Lookup()+OpenReader()+BeginWrite()+Invalidate()CacheManager+Lookup()+Merge()+Commit()«interface»ICacheStorage+OpenReader()+CreateWriter()«interface»IRemoteScanProvider+GetCapabilities()+Scan()+BatchGet()+GetSchema()GrpcProvider+Scan()+BatchGet()ArrowStreamReader+Next()+Close()

## 七、关键执行细节与数据正确性

上面的模块拆分还需要明确一些实现规则，否则即使类设计合理，也可能出现缓存错误。

### 7.1 Projection 与 Filter 的处理

定义：

```
Output Columns
    = 用户 SELECT 的列

Filter Columns
    = WHERE 使用的列

Required Columns
    = Output Columns ∪ Filter Columns
```

例如：

```
SELECT name
FROM hbase_scan(...)
WHERE age > 30;
```

Required Columns 必须包含：

```
name, age
```

如果用户执行：

```
SELECT name
FROM hbase_scan(...)
WHERE age > 30
LIMIT 10;
```

不能只缓存 LIMIT 10 返回的数据，然后将其当成完整 RowKey 范围的缓存。

第一版应禁止 LIMIT 影响 Cache 的数据完整性。

### 7.2 RowKey 语义

HBase RowKey 是 Binary，必须定义统一类型：

```
using RowKey = std::vector<uint8_t>;
```

范围必须定义：

```
struct RowKeyRange {
    RowKey start;
    RowKey stop;

    bool include_start;
    bool include_stop;
};
```

DuckDB 用户输入 VARCHAR 时按 UTF-8 转成字节；输入 BLOB 时保持原始字节。

缓存覆盖比较必须使用与 HBase 相同的字节序比较规则。

### 7.3 数据版本语义

建议定义：

```
struct ScanConsistency {
    optional<int64_t> min_timestamp;
    optional<int64_t> max_timestamp;

    int32_t max_versions;
    bool latest_only;
};
```

以下查询不能简单复用同一缓存：

```
-- 查询最新版本
SELECT *
FROM hbase_scan(...);

-- 查询历史版本
SELECT *
FROM hbase_scan(..., max_versions := 5);
```

同理，指定时间戳的 Scan 必须使用兼容的时间范围。

### 7.4 部分列补充

补列时必须以 RowKey 为主键。

推荐流程：

```
Cache Segment
     |
     v
读取 RowKey + 已缓存列
     |
     v
计算缺失列
     |
     v
Provider BatchGet
     |
     v
按 RowKey 对齐
     |
     v
补充 NULL / 实际 Cell
     |
     v
生成新 Segment
     |
     v
原子提交
```

特别需要区分：

- 列存在且值为空。
- 列不存在。
- RowKey 不存在。
- RowKey 在查询期间发生变化。

HBase Cell 不存在时应根据 HBase 的查询语义映射为 NULL，而不是误认为 RowKey 不存在。

如果 Batch Get 没有返回某个 RowKey，不能简单地将其作为完整的负结果缓存，因为这可能是并发删除、权限过滤或数据变化造成的。

### 7.5 并发查询

建议使用：

```
CacheManager
    |
    +-- Segment Lock
    +-- Column Lock
    +-- SingleFlight
    +-- Reader Reference Count
    +-- Generation CAS
```

例如：

```
Query A: Segment 1, missing age
Query B: Segment 1, missing age
Query C: Segment 1, missing city
```

A、B 合并同一个补列请求，C 可以独立请求 city。

提交时通过 Generation CAS 防止并发写覆盖。

### 7.6 查询失败恢复

缓存写入分成四个阶段：

```
INIT
  |
  v
WRITING
  |
  v
VALIDATING
  |
  v
COMMITTED
```

失败时：

- INIT：直接终止。
- WRITING：删除临时文件。
- VALIDATING：校验失败则 Abort。
- COMMITTED：元数据已生效，不回滚已发布 Segment。

查询取消时必须同时关闭远端 Scanner 和本地文件 Reader。

## 八、Gateway 运行、部署与安全设计

### 8.1 Gateway 部署拓扑

\#chatgpt-mermaid-\_r_1j9\_{font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:16px;fill:rgb(13, 13, 13);}@keyframes edge-animation-frame{from{stroke-dashoffset:0;}}@keyframes dash{to{stroke-dashoffset:0;}}#chatgpt-mermaid-\_r_1j9\_ .edge-animation-slow{stroke-dasharray:9,5!important;stroke-dashoffset:900;animation:dash 50s linear infinite;stroke-linecap:round;}#chatgpt-mermaid-\_r_1j9\_ .edge-animation-fast{stroke-dasharray:9,5!important;stroke-dashoffset:900;animation:dash 20s linear infinite;stroke-linecap:round;}#chatgpt-mermaid-\_r_1j9\_ .error-icon{fill:rgb(243, 243, 243);}#chatgpt-mermaid-\_r_1j9\_ .error-text{fill:rgb(13, 13, 13);stroke:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1j9\_ .edge-thickness-normal{stroke-width:1px;}#chatgpt-mermaid-\_r_1j9\_ .edge-thickness-thick{stroke-width:3.5px;}#chatgpt-mermaid-\_r_1j9\_ .edge-pattern-solid{stroke-dasharray:0;}#chatgpt-mermaid-\_r_1j9\_ .edge-thickness-invisible{stroke-width:0;fill:none;}#chatgpt-mermaid-\_r_1j9\_ .edge-pattern-dashed{stroke-dasharray:3;}#chatgpt-mermaid-\_r_1j9\_ .edge-pattern-dotted{stroke-dasharray:2;}#chatgpt-mermaid-\_r_1j9\_ .marker{fill:rgb(143, 143, 143);stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_1j9\_ .marker.cross{stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_1j9\_ svg{font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:16px;}#chatgpt-mermaid-\_r_1j9\_ p{margin:0;}#chatgpt-mermaid-\_r_1j9\_ .label{font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1j9\_ .cluster-label text{fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1j9\_ .cluster-label span{color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1j9\_ .cluster-label span p{background-color:transparent;}#chatgpt-mermaid-\_r_1j9\_ .label text,#chatgpt-mermaid-\_r_1j9\_ span{fill:rgb(13, 13, 13);color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1j9\_ .node rect,#chatgpt-mermaid-\_r_1j9\_ .node circle,#chatgpt-mermaid-\_r_1j9\_ .node ellipse,#chatgpt-mermaid-\_r_1j9\_ .node polygon,#chatgpt-mermaid-\_r_1j9\_ .node path{fill:rgb(222, 234, 251);stroke:rgb(83, 154, 248);stroke-width:1px;}#chatgpt-mermaid-\_r_1j9\_ .rough-node .label text,#chatgpt-mermaid-\_r_1j9\_ .node .label text,#chatgpt-mermaid-\_r_1j9\_ .image-shape .label,#chatgpt-mermaid-\_r_1j9\_ .icon-shape .label{text-anchor:middle;}#chatgpt-mermaid-\_r_1j9\_ .node .katex path{fill:#000;stroke:#000;stroke-width:1px;}#chatgpt-mermaid-\_r_1j9\_ .rough-node .label,#chatgpt-mermaid-\_r_1j9\_ .node .label,#chatgpt-mermaid-\_r_1j9\_ .image-shape .label,#chatgpt-mermaid-\_r_1j9\_ .icon-shape .label{text-align:center;}#chatgpt-mermaid-\_r_1j9\_ .node.clickable{cursor:pointer;}#chatgpt-mermaid-\_r_1j9\_ .root .anchor path{fill:rgb(143, 143, 143)!important;stroke-width:0;stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_1j9\_ .arrowheadPath{fill:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_1j9\_ .edgePath .path{stroke:rgb(143, 143, 143);stroke-width:1px;}#chatgpt-mermaid-\_r_1j9\_ .flowchart-link{stroke:rgb(143, 143, 143);fill:none;}#chatgpt-mermaid-\_r_1j9\_ .edgeLabel{background-color:rgb(252, 252, 252);text-align:center;}#chatgpt-mermaid-\_r_1j9\_ .edgeLabel p{background-color:rgb(252, 252, 252);}#chatgpt-mermaid-\_r_1j9\_ .edgeLabel rect{opacity:0.5;background-color:rgb(252, 252, 252);fill:rgb(252, 252, 252);}#chatgpt-mermaid-\_r_1j9\_ .labelBkg{background-color:rgba(252, 252, 252, 0.5);}#chatgpt-mermaid-\_r_1j9\_ .cluster rect{fill:rgb(243, 243, 243);stroke:rgba(0, 0, 0, 0.1);stroke-width:1px;}#chatgpt-mermaid-\_r_1j9\_ .cluster text{fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1j9\_ .cluster span{color:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1j9\_ div.mermaidTooltip{position:absolute;text-align:center;max-width:200px;padding:2px;font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";font-size:12px;background:rgb(243, 243, 243);border:1px solid rgba(0, 0, 0, 0.1);border-radius:2px;pointer-events:none;z-index:100;}#chatgpt-mermaid-\_r_1j9\_ .flowchartTitleText{text-anchor:middle;font-size:18px;fill:rgb(13, 13, 13);}#chatgpt-mermaid-\_r_1j9\_ rect.text{fill:none;stroke-width:0;}#chatgpt-mermaid-\_r_1j9\_ .icon-shape,#chatgpt-mermaid-\_r_1j9\_ .image-shape{background-color:rgb(252, 252, 252);text-align:center;}#chatgpt-mermaid-\_r_1j9\_ .icon-shape p,#chatgpt-mermaid-\_r_1j9\_ .image-shape p{background-color:rgb(252, 252, 252);padding:2px;}#chatgpt-mermaid-\_r_1j9\_ .icon-shape .label rect,#chatgpt-mermaid-\_r_1j9\_ .image-shape .label rect{opacity:0.5;background-color:rgb(252, 252, 252);fill:rgb(252, 252, 252);}#chatgpt-mermaid-\_r_1j9\_ .label-icon{display:inline-block;height:1em;overflow:visible;vertical-align:-0.125em;}#chatgpt-mermaid-\_r_1j9\_ .node .label-icon path{fill:currentColor;stroke:revert;stroke-width:revert;}#chatgpt-mermaid-\_r_1j9\_ .node .neo-node{stroke:rgb(83, 154, 248);}#chatgpt-mermaid-\_r_1j9\_ [data-look="neo"].node rect,#chatgpt-mermaid-\_r_1j9\_ [data-look="neo"].cluster rect,#chatgpt-mermaid-\_r_1j9\_ [data-look="neo"].node polygon{stroke:url(#chatgpt-mermaid-\_r_1j9\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1j9\_ [data-look="neo"].swimlane.cluster rect{filter:none;}#chatgpt-mermaid-\_r_1j9\_ [data-look="neo"].node path{stroke:url(#chatgpt-mermaid-\_r_1j9\_-gradient);stroke-width:1px;}#chatgpt-mermaid-\_r_1j9\_ [data-look="neo"].node .outer-path{filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1j9\_ [data-look="neo"].node .neo-line path{stroke:rgb(83, 154, 248);filter:none;}#chatgpt-mermaid-\_r_1j9\_ [data-look="neo"].node circle{stroke:url(#chatgpt-mermaid-\_r_1j9\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1j9\_ [data-look="neo"].node circle .state-start{fill:#000000;}#chatgpt-mermaid-\_r_1j9\_ [data-look="neo"].icon-shape .icon{fill:url(#chatgpt-mermaid-\_r_1j9\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1j9\_ [data-look="neo"].icon-shape .icon-neo path{stroke:url(#chatgpt-mermaid-\_r_1j9\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_1j9\_ .node text{font-size:14px;font-weight:600;letter-spacing:normal;fill:rgb(0, 79, 153);}#chatgpt-mermaid-\_r_1j9\_ .edgeLabels text{font-size:13px;font-weight:600;letter-spacing:-0.08px;fill:rgb(0, 79, 153);}#chatgpt-mermaid-\_r_1j9\_ .node tspan[font-weight="normal"],#chatgpt-mermaid-\_r_1j9\_ .edgeLabels tspan[font-weight="normal"]{font-weight:600;}#chatgpt-mermaid-\_r_1j9\_ .edgeLabel .label rect{opacity:1;rx:13px;ry:13px;fill:rgb(245, 250, 255);stroke:rgb(206, 219, 229);stroke-width:1px;}#chatgpt-mermaid-\_r_1j9\_ .node rect,#chatgpt-mermaid-\_r_1j9\_ .node circle,#chatgpt-mermaid-\_r_1j9\_ .node ellipse,#chatgpt-mermaid-\_r_1j9\_ .node polygon,#chatgpt-mermaid-\_r_1j9\_ .node path{fill:rgb(229, 243, 255);stroke:rgba(0, 0, 0, 0.1);stroke-width:1px;}#chatgpt-mermaid-\_r_1j9\_ .node rect{rx:16px;ry:16px;}#chatgpt-mermaid-\_r_1j9\_ .mermaid-decision .label-container{fill:rgb(245, 250, 255);stroke:rgb(206, 219, 229);stroke-dasharray:2,2;}#chatgpt-mermaid-\_r_1j9\_ .edgePaths .flowchart-link{stroke:rgb(143, 143, 143);stroke-width:1px;stroke-linecap:round;stroke-linejoin:round;}#chatgpt-mermaid-\_r_1j9\_ .marker{fill:rgb(143, 143, 143);stroke:rgb(143, 143, 143);}#chatgpt-mermaid-\_r_1j9\_ :root{--mermaid-font-family:-apple-system-body,ui-sans-serif,-apple-system,system-ui,"Segoe UI",Helvetica,"Apple Color Emoji",Arial,sans-serif,"Segoe UI Emoji","Segoe UI Symbol";}HBase 集群Gateway 集群分析节点ZooKeeperRegionServer 1RegionServer 2RegionServer 3Load BalancerGateway Instance 1Gateway Instance 2Gateway Instance 3DuckDB Process 1DuckDB Process 2Local Scan Cache

生产环境可以部署多个 Gateway 实例，但要注意：

- Extension 本地 Cache 默认属于单个 DuckDB 进程。
- 不同分析节点之间不自动共享 Cache。
- Gateway 实例之间需要共享连接配置和认证策略。
- 远端 Scan 请求应携带 Request ID，便于取消和追踪。

### 8.2 安全

必须支持：

1. Kerberos 认证。
2. TLS。
3. Gateway API 认证。
4. HBase Namespace / Table 权限控制。
5. SQL 参数与配置的安全处理。
6. 审计日志。

Gateway 不应接受客户端随意传入 Java 类名、SQL 拼接片段或任意 HBase Filter 序列化对象。

Phoenix SQL 必须采用参数绑定。

### 8.3 超时和重试

| 操作        | 策略            |
| --------- | ------------- |
| Schema 查询 | 可有限重试         |
| Scan 建立   | 建立失败可以重试      |
| Scan 流读取  | 需谨慎，避免重复返回数据  |
| Batch Get | 按 RowKey 批次重试 |
| Cache 写入  | 允许 Abort 后重试  |
| Cancel    | 幂等            |

不能对一个已经返回部分结果的 Scan 直接从头重试，再将新数据拼接到旧数据中。

如果未来支持断点续传，需要引入最后处理的 RowKey 和稳定的扫描语义。

## 九、测试与质量保障

### 9.1 单元测试

| 模块              | 测试重点                    |
| --------------- | ----------------------- |
| RowKey          | Binary 排序、范围边界          |
| ScanPlanner     | Projection、Filter、Range |
| CacheLookup     | 全命中、部分命中、范围缺口           |
| CacheMerger     | RowKey 对齐、NULL          |
| CacheStorage    | 文件损坏、原子提交               |
| Provider        | 能力协商、错误映射               |
| Gateway Adapter | Scan、Get 转换             |

### 9.2 集成测试

需要真实 HBase 2.x 环境，至少覆盖：

- Native Java Adapter。
- HBase REST Adapter。
- Thrift Adapter（如果纳入首批支持）。
- Phoenix Adapter（独立语义测试）。

### 9.3 性能测试

至少对比四组数据：

1. 直接 HBase Scan。
2. 首次 Scan + Cache Write。
3. 第二次 Full Cache Hit。
4. 第二次 Partial Cache Hit。

指标：

| 指标                     | 目标         |
| ---------------------- | ---------- |
| Query Latency          | 查询延迟       |
| HBase Scan Count       | 远端 Scan 次数 |
| HBase Get Count        | 补列 Get 次数  |
| Cache Hit Rate         | 缓存命中率      |
| Memory Usage           | 内存占用       |
| Cache Write Throughput | 缓存写入吞吐     |
| Gateway Throughput     | Gateway 吞吐 |

需要通过真实数据测量，不建议在设计阶段预设固定加速倍数。

## 十、开发阶段与交付标准

建议分为五个里程碑。

| 阶段 | 核心交付                       | 验收标准                         |
| -- | -------------------------- | ---------------------------- |
| M1 | Extension + Native Gateway | 能执行真实 HBase Scan             |
| M2 | Provider 抽象 + gRPC/Arrow   | Extension 不依赖 Native Adapter |
| M3 | Full Cache Hit             | 重复查询不访问 HBase                |
| M4 | Partial Cache Hit          | 增加列可以按 RowKey 补齐             |
| M5 | 多 Adapter + 生产优化           | 支持多种接入方式、并发与失效               |

### M1：基础闭环

- DuckDB C++ Extension。
- Java Gateway。
- Native HBase Adapter。
- `hbase_scan()`。
- Arrow IPC Streaming。
- Python 和 CLI 使用示例。

### M2：接口解耦

- `IRemoteScanProvider`。
- Gateway SPI。
- Provider Capabilities。
- gRPC 协议。
- Native Java Adapter 独立模块。

### M3：缓存闭环

- Segment。
- Coverage Index。
- Parquet。
- Metadata。
- Full Hit。
- TTL。
- 缓存统计。

### M4：增量补列

- Partial Hit。
- Batch Get。
- RowKey Merger。
- Copy-on-Write。
- 并发请求合并。

### M5：生产级扩展

- REST / Thrift / Phoenix Adapter。
- Kerberos。
- TLS。
- 缓存淘汰。
- 监控与审计。
- 多平台构建和发行。

## 十一、最终技术选型

| 领域              | 推荐选型                     | 说明           |
| --------------- | ------------------------ | ------------ |
| Extension       | C++                      | DuckDB 原生扩展  |
| SQL API         | Table Function           | 暂不实现 Catalog |
| HBase Client    | Java HBase Client 2.x    | 默认 Gateway   |
| Gateway         | Java                     | 独立部署         |
| RPC             | gRPC                     | 统一传输协议       |
| Schema Protocol | Protobuf                 | 接口定义         |
| 数据传输            | Arrow IPC                | 流式列式数据       |
| Cache           | Parquet                  | 持久化列式存储      |
| Cache Metadata  | SQLite                   | 轻量索引         |
| 热点缓存            | Memory                   | L1 Cache     |
| Build           | CMake + Maven            | 分离构建         |
| 测试              | DuckDB SQL Tests + JUnit | 双侧测试         |

DuckDB C++ Extension API 目前仍需要关注版本兼容性。建议以官方 Extension Template 为基础，锁定一个 DuckDB 版本和对应源码 Commit，避免在开发过程中不断跟随主干变动。官方也明确提示 C++ API 不是稳定 ABI。

[image](https://www.google.com/s2/favicons?domain=https://duckdb.org\&sz=32)

DuckDB+2



官方安装与加载机制支持本地 Extension 文件和 Extension Repository 两种方式。

[image](https://www.google.com/s2/favicons?domain=https://duckdb.org\&sz=32)

DuckDB+1



## 十二、正式技术方案文档

以上内容已经明确了架构、接口和工程边界。下面将核心设计归纳为可以直接进入项目文档库的正式版本。

# DuckDB-HBase Extension 完整技术设计方案

**版本：** V1.0\
**日期：** 2026-11-01\
**项目：** duckdb-hbase

## 1. 项目定位

构建一个面向 HBase 的 DuckDB 外部数据源扩展，提供 SQL 查询、列式 Scan Cache 和多种 HBase 接入方式适配能力。

核心目标：

1. 通过 DuckDB SQL 查询 HBase。
2. 对执行过的 Scan 数据进行本地持久化缓存。
3. 支持不同 SQL 之间的数据列复用。
4. 支持 Full Hit、Partial Hit、Cache Miss。
5. 通过统一 Provider Protocol 解耦 Extension 与 Gateway。
6. 支持 Native Java、REST、Thrift、Phoenix 等接入方式。
7. 提供简单的 SQL、Python API。

技术约束：

- HBase 2.x。
- 自建 Hadoop/HBase 集群。
- 独立 Java Gateway。
- C++ DuckDB Extension。
- 初期不实现 DuckDB Catalog。
- 第一阶段只读。
- Arrow IPC 流式数据交换。

## 2. 总体架构

系统分为四层：

### 2.1 DuckDB Extension

包括：

- Extension Registry
- SQL API
- Scan Planner
- Scan Executor
- Cache Manager
- Cache Storage
- Provider Client
- Schema Metadata

### 2.2 Remote Provider Protocol

定义统一的：

- Scan
- BatchGet
- GetSchema
- GetCapabilities
- Cancel
- Health

### 2.3 Gateway Adapter

通过 SPI 支持：

- NativeHBaseAdapter
- RestHBaseAdapter
- ThriftHBaseAdapter
- PhoenixAdapter
- CustomAdapter

### 2.4 HBase Infrastructure

实际访问 HBase 2.x 集群。

## 3. Extension 模块设计

采用以下模块：

| 模块        | 主要职责                    |
| --------- | ----------------------- |
| extension | 扩展加载、函数注册               |
| sql       | SQL Table Function      |
| scan      | Scan 规划与执行              |
| cache     | 缓存查询、覆盖判断、补列            |
| storage   | Parquet、Memory、Metadata |
| provider  | 远程 Provider Client      |
| catalog   | HBase Schema 和列类型映射     |
| common    | 配置、类型、日志、异常             |

核心类：

- HBaseExtension
- HBaseScanFunction
- HBaseGetFunction
- ScanPlanner
- ScanExecutor
- ScanPipeline
- CacheManager
- CacheLookupEngine
- CacheCoverageIndex
- CacheSegmentManager
- CacheMerger
- CacheInvalidationManager
- ParquetCacheStorage
- MemoryCacheStorage
- GatewayProviderFactory

### 3.1 Scan 执行

SQL Bind → Schema 获取 → ScanPlan → Projection / Filter 解析 → Cache Lookup → Cache 或 Gateway → DataChunk → DuckDB Execution。

### 3.2 ScanPlan

至少包含：

- TableIdentifier
- RequiredColumns
- OutputColumns
- Filters
- RowKeyRanges
- ScanConsistency
- CachePolicy

### 3.3 Cache 抽象

ICacheManager：

- Lookup
- OpenReader
- BeginWrite
- Invalidate
- GetStatistics

ICacheReader：

- Next
- Close

ICacheWriter：

- Append
- Commit
- Abort

### 3.4 缓存规则

完整命中要求：

- RowKey 范围完整覆盖。
- RequiredColumns 全部覆盖。
- 数据时间戳语义兼容。
- 缓存未过期。
- 缓存数据完整。

缓存以 Segment 为单位管理，不能简单以 SQL Hash 为 Key。

Cache 分为：

- L1：Memory Cache
- L2：Parquet Cache
- Metadata：SQLite

部分列补齐必须以 RowKey 对齐，采用 Copy-on-Write 和原子 Metadata Commit。

## 4. Gateway 抽象设计

Extension 只依赖 IRemoteScanProvider，不直接依赖 gRPC、HTTP 或 Java Gateway 的具体实现。

### 4.1 Provider 接口

- GetCapabilities()
- Scan()
- BatchGet()
- GetSchema()
- Cancel()
- CheckHealth()

### 4.2 Provider Capabilities

描述：

- 是否支持 Scan。
- 是否支持 Batch Get。
- 是否支持 RowKey Range。
- 是否支持 Column Projection。
- 是否支持 Timestamp。
- 是否支持 Versions。
- 是否支持 Filter Pushdown。
- 是否支持 Cancellation。

### 4.3 Gateway SPI

统一定义 HBaseAccessAdapter：

- capabilities()
- getSchema()
- scan()
- batchGet()
- cancel()
- health()

### 4.4 Adapter 实现

Native Java Adapter 为默认实现，使用 HBase Java Client 2.x。

REST、Thrift、Phoenix 独立实现 Adapter，不影响 Extension。

Phoenix 需要明确区分 SQL 查询语义与原生 HBase Cell Scan 语义。

### 4.5 通信协议

推荐：

- gRPC Streaming
- Protobuf
- Arrow IPC

协议对象：

- ScanRequest
- BatchGetRequest
- SchemaRequest
- CapabilityResponse
- ArrowBatch
- ErrorResponse

RowKey 采用 Binary 类型，不能默认按 UTF-8 字符串处理。

## 5. 用户 API

### 5.1 连接

```
SELECT hbase_connect(
    'prod',
    'grpc://127.0.0.1:18080'
);
```

### 5.2 Scan

```
SELECT *
FROM hbase_scan(
    'prod',
    'default',
    'customer_profile'
);
```

支持：

- start_row
- stop_row
- columns
- max_versions
- timestamp_min
- timestamp_max
- cache

### 5.3 Get

```
SELECT *
FROM hbase_get(
    'prod',
    'default',
    'customer_profile',
    rowkeys := ['1001', '1002']
);
```

### 5.4 Cache 管理

```
SELECT * FROM hbase_cache_stats();

SELECT hbase_cache_clear(
    'prod',
    'default',
    'customer_profile'
);
```

### 5.5 配置

```
SET hbase_cache_enabled = true;
SET hbase_cache_ttl = 300;
SET hbase_cache_max_size = '10GB';
SET hbase_cache_directory = './hbase_cache';
```

## 6. 工程分包

采用 Monorepo：

```
duckdb-hbase/
├── extension/
│   ├── src/
│   │   ├── extension/
│   │   ├── sql/
│   │   ├── scan/
│   │   ├── cache/
│   │   ├── storage/
│   │   ├── provider/
│   │   ├── catalog/
│   │   └── common/
│   ├── include/
│   ├── test/
│   └── CMakeLists.txt
├── gateway/
│   ├── gateway-api/
│   ├── gateway-core/
│   ├── adapter-native/
│   ├── adapter-rest/
│   ├── adapter-thrift/
│   ├── adapter-phoenix/
│   └── gateway-server/
├── protocol/
├── examples/
├── scripts/
└── docs/
```

Extension 使用 CMake，Gateway 使用 Maven。

Gateway Adapter 之间只依赖统一 SPI，不允许交叉依赖具体实现。

## 7. 部署方案

用户侧：

1. 安装 DuckDB。
2. 安装 duckdb_hbase Extension。
3. 部署 Java Gateway。
4. 配置 HBase 连接。
5. LOAD Extension。
6. 注册连接。
7. 执行 SQL。

开发阶段支持本地 Extension 文件加载：

```
LOAD '/opt/duckdb/extensions/hbase.duckdb_extension';
```

未来发布到 Community Repository 后支持：

```
INSTALL hbase FROM community;
LOAD hbase;
```

Python 同样支持通过 `duckdb.connect()` 加载 Extension。

## 8. 一致性与安全

缓存支持：

- TTL。
- 手动失效。
- 后续 CDC 失效。

必须区分缓存覆盖完整性和数据新鲜度。

生产环境支持：

- Kerberos。
- TLS。
- Gateway 认证。
- HBase 权限校验。
- 审计。
- 请求限流。

## 9. 测试

单元测试：

- RowKey。
- ScanPlanner。
- CacheLookup。
- CacheMerger。
- CacheStorage。
- Provider。
- Gateway Adapter。

集成测试：

- Native Java。
- REST。
- Thrift。
- Phoenix。

核心验收：

- 相同 Scan 二次查询 Full Hit。
- 增加列时 Partial Hit。
- 不同过滤条件正确复用。
- RowKey 子范围正确命中。
- 过期缓存不能错误命中。
- 并发补列不会重复写入错误数据。
- 查询取消可以正确释放远端资源。

## 10. 研发里程碑

| 阶段 | 交付                                     |
| -- | -------------------------------------- |
| M1 | Extension + Native Java Gateway + Scan |
| M2 | Provider 抽象 + gRPC + Arrow             |
| M3 | Full Cache Hit + Parquet               |
| M4 | Partial Hit + BatchGet + RowKey Merge  |
| M5 | 多 Adapter + 生产优化                       |

## 11. 核心设计原则

1. Extension 与 Gateway 完全解耦。
2. Cache Engine 不依赖具体 HBase 接入方式。
3. Scan Segment 是缓存基本管理单元。
4. 所有部分补列以 RowKey 为关联键。
5. Arrow 流式传输，禁止全量结果无界加载内存。
6. DuckDB 负责 SQL 执行，Gateway 负责远端数据访问。
7. 第一阶段优先完成 Native Java Adapter，不提前实现所有 Adapter。
8. 缓存必须保证完整性校验、原子提交和一致性策略。

**最终定位：DuckDB-HBase 不只是一个 SQL Connector，而是一个具备多接入适配能力和本地列式 Scan Cache 的 HBase 分析型数据访问引擎。**

## 十三、我建议下一步优先解决的三个实现问题

1. DuckDB Extension 的 Scan API 实现。 特别是 Table Function 的 Projection Pushdown、Filter Pushdown、Global/Local State，以及并行 Scan 的具体实现。
2. Provider Protocol 的正式定义。 确定 Protobuf、Arrow IPC、错误模型、能力协商以及 Gateway Adapter SPI，避免后续出现 C++ 和 Java 两套模型不一致。
3. Cache Coverage Index 的算法。 这是最有技术含量的部分，需要解决多 Segment、多列、多 RowKey 范围之间的覆盖关系。

我建议下一步直接进入第一个问题：基于 DuckDB Extension Template，给出 `hbase_scan()` 的完整 C++ 类设计、调用链和核心接口代码，并使它能够与上述 Cache Manager、Provider 接口无缝衔接。

---

## 附录 A：设计修订记录（ADR）

### ADR-001：数据面编码在 M2 阶段采用 UTF8 列式载荷，Arrow IPC 延后

- **背景**：本方案 §3.5 约定 `ArrowBatch.ipc_payload` 承载 Arrow IPC。实现阶段发现 C++ 侧需要 Arrow C++ 依赖，而 vcpkg 构建 arrow 时在 boost/thrift 下载与编译阶段长时间失败（网络与体积原因），阻塞了端到端交付。
- **决策**：M1/M2 的数据面使用 `BATCH_ENCODING_UTF8_COLUMNS`（magic + 列名 + 长度表 + 数据，见 `docs/protocol.md` §3.4）；协议保留 `ArrowBatch.encoding` 字段，后续切换 Arrow IPC 不需要破坏性变更。
- **影响**：所有列以 UTF8 字符串承载，NULL 用长度 `-1` 表示；类型映射与多版本展开在这一编码上实现。
- **回退条件**：当 Arrow C++ 依赖可以稳定构建（或采用预编译包）时，新增 `BATCH_ENCODING_ARROW_IPC` 编码并让 Gateway 按能力协商结果选择编码；Extension 侧按 `encoding` 分派解码器。
- **状态**：已生效（2026-11），Arrow IPC 列在 `docs/roadmap.md` 技术债。
