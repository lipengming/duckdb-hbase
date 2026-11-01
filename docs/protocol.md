# Gateway 通信协议

> 提炼自 [技术方案 V1.0](../tech_v0.1.md) §3。若与方案冲突，以方案为准。

本文定义 Extension 与 Gateway 之间的统一远程协议。Gateway 抽象与 Adapter 装配见 [gateway-design.md](gateway-design.md)，分层边界见 [architecture.md](architecture.md)，Extension 侧调用见 [extension-design.md](extension-design.md)，缓存与补列见 [cache-design.md](cache-design.md)，部署与安全见 [deployment-guide.md](deployment-guide.md)。可编译定义见 [hbase_data_service.proto](../protocol/proto/hbase_data_service.proto)。能力与错误部分同时参考方案 §4.5、§8.3。

## 1. 协议定位

- 控制面：Protobuf 定义请求、响应、能力、Schema、取消与健康检查语义，经 gRPC 传输。
- 数据面：Scan / BatchGet 返回 `stream ArrowBatch`，数据本体是 Arrow IPC 载荷。
- 协议表达业务级 Scan 语义，不暴露 Java HBase Client 的内部对象（§3.5）。
- RowKey 一律按 Binary 处理，不能默认 UTF-8；直接转字符串可能破坏排序和范围比较语义。
- 方案推荐组合：gRPC Streaming + Protobuf + Arrow IPC（§4.5）。

协议不承担 SQL 解析、Filter 求值、缓存判断等职责，这些属于 Extension。

## 2. 服务与 RPC

服务名 `HBaseDataService`，包名 `duckdb.hbase.v1`。

| RPC | 请求 | 响应 | 流式 | 用途 |
| --- | --- | --- | --- | --- |
| `GetCapabilities` | `CapabilityRequest` | `CapabilityResponse` | 否 | 能力协商 |
| `GetSchema` | `SchemaRequest` | `SchemaResponse` | 否 | 获取表 Schema 与逻辑类型 |
| `Scan` | `ScanRequest` | `stream ArrowBatch` | 服务端流 | 范围扫描 |
| `BatchGet` | `BatchGetRequest` | `stream ArrowBatch` | 服务端流 | 按 RowKey 批量读取 |
| `Cancel` | `CancelRequest` | `CancelResponse` | 否 | 取消指定请求 |
| `Health` | `HealthRequest` | `HealthResponse` | 否 | 健康检查 |

## 3. 请求与响应字段

### 3.1 ColumnRef

列引用，由 Family 与 Qualifier 组成。

| 字段 | 编号 | 类型 | 说明 |
| --- | --- | --- | --- |
| `family` | 1 | `string` | 列族 |
| `qualifier` | 2 | `string` | 列限定符 |

### 3.2 ScanRequest

| 字段 | 编号 | 类型 | 说明 |
| --- | --- | --- | --- |
| `request_id` | 1 | `string` | 请求唯一标识，用于取消与追踪 |
| `namespace` | 2 | `string` | HBase Namespace |
| `table` | 3 | `string` | 表名 |
| `start_row` | 4 | `bytes` | 起始 RowKey，二进制 |
| `stop_row` | 5 | `bytes` | 结束 RowKey，二进制 |
| `include_start` | 6 | `bool` | 是否包含起始 RowKey |
| `include_stop` | 7 | `bool` | 是否包含结束 RowKey |
| `columns` | 8 | `repeated ColumnRef` | 列裁剪列表 |
| `min_timestamp` | 9 | `int64` | 最小时间戳，单位待定（TBD） |
| `max_timestamp` | 10 | `int64` | 最大时间戳，单位待定（TBD） |
| `max_versions` | 11 | `int32` | 最大版本数 |
| `batch_size` | 12 | `int32` | 期望批大小，受服务端与能力上限约束 |

### 3.3 BatchGetRequest

| 字段 | 编号 | 类型 | 说明 |
| --- | --- | --- | --- |
| `request_id` | 1 | `string` | 请求唯一标识，用于取消与追踪 |
| `namespace` | 2 | `string` | HBase Namespace |
| `table` | 3 | `string` | 表名 |
| `rowkeys` | 4 | `repeated bytes` | 待读取 RowKey 列表，二进制 |
| `columns` | 5 | `repeated ColumnRef` | 列裁剪列表 |
| `min_timestamp` | 6 | `int64` | 最小时间戳，单位待定（TBD） |
| `max_timestamp` | 7 | `int64` | 最大时间戳，单位待定（TBD） |

### 3.4 ArrowBatch

| 字段 | 编号 | 类型 | 说明 |
| --- | --- | --- | --- |
| `ipc_payload` | 1 | `bytes` | Arrow IPC 数据载荷；**每个批次是一条完整的 IPC stream**（schema + 1 个 RecordBatch + EOS），客户端可逐批独立解码 |
| `sequence_number` | 2 | `uint64` | 批次序号，用于顺序校验与续传 |
| `finished` | 3 | `bool` | 是否为本次请求的最后一批 |
| `row_count` | 4 | `uint64` | 本批行数 |

### 3.5 CapabilityResponse

逐项对应方案 §3.4 的 `ProviderCapabilities`；Extension 必须以此为准，不得假定所有适配器能力一致。

| 字段 | 编号 | 类型 | 说明 |
| --- | --- | --- | --- |
| `supports_scan` | 1 | `bool` | 是否支持 Scan |
| `supports_batch_get` | 2 | `bool` | 是否支持 Batch Get；不支持时 Partial Hit 退化为范围 Scan |
| `supports_rowkey_range` | 3 | `bool` | 是否支持 RowKey 范围扫描 |
| `supports_column_projection` | 4 | `bool` | 是否支持列裁剪 |
| `supports_timestamp` | 5 | `bool` | 是否支持时间戳过滤 |
| `supports_versions` | 6 | `bool` | 是否支持多版本 |
| `supports_filter_pushdown` | 7 | `bool` | 是否支持 Filter 下推 |
| `supports_async_scan` | 8 | `bool` | 是否支持异步 Scan |
| `supports_cancellation` | 9 | `bool` | 是否支持取消 |
| `max_batch_size` | 10 | `int32` | 单批最大行数 |
| `max_parallel_scans` | 11 | `int32` | 单连接最大并行 Scan 数 |

`CapabilityRequest` 无字段。

### 3.6 SchemaRequest / ColumnSchema / SchemaResponse

| 消息 | 字段 | 编号 | 类型 | 说明 |
| --- | --- | --- | --- | --- |
| `SchemaRequest` | `namespace` | 1 | `string` | HBase Namespace |
| `SchemaRequest` | `table` | 2 | `string` | 表名 |
| `ColumnSchema` | `family` | 1 | `string` | 列族 |
| `ColumnSchema` | `qualifier` | 2 | `string` | 列限定符；为空表示整列族 |
| `ColumnSchema` | `sql_type` | 3 | `string` | DuckDB LogicalType 名称（如 `VARCHAR`、`BIGINT`） |
| `SchemaResponse` | `columns` | 1 | `repeated ColumnSchema` | 列列表 |

说明：HBase 是 schemaless，列族以外的 qualifier 无法从 `TableDescriptor` 获得；Native Adapter 当前只返回列族（`qualifier` 为空），具体列在首次读取时发现。类型推断规则见 §3.8。

### 3.7 CancelRequest / CancelResponse / HealthRequest / HealthResponse

| 消息 | 字段 | 编号 | 类型 | 说明 |
| --- | --- | --- | --- | --- |
| `CancelRequest` | `request_id` | 1 | `string` | 待取消的请求 ID |
| `CancelResponse` | `cancelled` | 1 | `bool` | 是否确实取消了仍在执行的请求（幂等） |
| `HealthRequest` | — | — | — | 无字段 |
| `HealthResponse` | `status` | 1 | `string` | `serving` / `not_serving` / `unknown` |
| `HealthResponse` | `version` | 2 | `string` | Gateway 版本 |

### 3.8 类型映射（M1 约定）

M1 数据面统一使用 **UTF8 字符串列**：Cell 不存在写 NULL，存在则按 UTF-8 解码；`sql_type` 恒为 `VARCHAR`。`BIGINT`/`DOUBLE`/`TIMESTAMP` 等映射规则仍待定（TBD），需要业务侧的列类型声明，见 §3.9。

### 3.9 类型映射（待定）

`sql_type` 与 HBase 字节值的映射规则尚未定稿（TBD）：M1 先统一按 `VARCHAR` 处理，后续按 HBase `Bytes` 工具与业务约定补齐 `BIGINT`/`DOUBLE`/`BOOLEAN` 等映射。

### 3.9 尚未定义的消息

`ErrorResponse` 在方案 §4.5 中仅列名字，字段与错误码仍未定义（TBD）。M1 阶段 gRPC 错误通过 `Status`（如 `UNIMPLEMENTED` / `INTERNAL`）返回，生产实现前须补充正式错误模型。

### 3.10 生成代码约定

`.proto` 声明 `option java_multiple_files = true;`，Java 生成类直接位于 `com.duckdb.hbase.protocol.v1` 包下，便于按类导入。

## 4. 错误模型

原则：协议返回业务级错误，不把 Java 异常或 HBase 客户端对象直接透传；错误携带请求 ID 与可重试性，便于 Extension 决定退避或降级。具体错误码取值与 `ErrorResponse` 字段待定（TBD）。

| 错误类别 | 典型场景 | 可重试性 | 处理建议 |
| --- | --- | --- | --- |
| 请求参数 / 协议错误 | 字段非法、RowKey 编码错误、版本不匹配 | 不可重试 | 修正请求后重发 |
| 认证 / 授权错误 | Kerberos 失败、Namespace / Table 越权 | 不可重试 | 重新认证或申请权限，不能原样重试 |
| 能力不支持 | 不支持 Batch Get、时间戳或版本语义 | 不可重试 | 按能力协商结果降级 |
| Schema / 元数据查询失败 | 元数据服务暂时不可用 | 可有限重试 | 限制次数并退避 |
| Scan 建立失败 | 建连、建 Scanner 失败且尚未返回数据 | 可重试 | 仅在未返回数据时重试 |
| Scan 流中断 | 已返回部分批次后连接断开 | 不可直接重试 | 禁止静默从头重扫并拼接旧数据 |
| Batch Get 失败 | 部分 RowKey 读取失败 | 可按 RowKey 批次重试 | 只重试失败批次，避免重复数据 |
| 超时 / 限流 / 资源不足 | Deadline 到期、服务端限流 | 可有限重试 | 退避后重试，受 Deadline 约束 |
| 内部错误 | Gateway 未预期异常 | 默认不可重试 | 记录 Trace 并返回明确错误 |

## 5. 能力协商流程

1. 用户通过 `hbase_connect` 注册 Gateway 地址，Extension 建立会话。
2. Extension 调用 `GetCapabilities`，获取 Gateway 当前能力。
3. Extension 将响应映射为 `ProviderCapabilities`（§3.4），并校验关键能力。
4. 规划阶段按能力决策：
   - 不支持 Batch Get 时，Partial Hit 退化为范围 Scan。
   - 不支持时间戳时，不能把该 Provider 当作支持特定 HBase 时间戳语义。
   - `batch_size` 不超过 `max_batch_size`；并行 Scan 不超过 `max_parallel_scans`。
   - 不支持 Filter Pushdown 时，Filter 仅在 Extension 本地执行。
5. 能力结果随会话缓存，重连时重新协商；刷新策略待定（TBD）。
6. Adapter SPI 版本兼容性检查在会话建立时进行，版本号形式与协商字段待定（TBD）。

能力协商是必需的：不同接入方式能力不同，不能假定所有 Gateway 都支持同样的功能（§3.4）。

## 6. 流式传输与背压

- `Scan` / `BatchGet` 使用服务端流，每个 `ArrowBatch` 是一个独立的 Arrow IPC 载荷。
- `sequence_number` 单调递增，用于顺序校验；`finished=true` 表示流正常结束，其后不应再有批次。
- 消费端按批拉取，处理完一批再取下一批；Gateway 不得在服务端累积完整结果集。
- `batch_size` 是期望值而非强制值，实际批大小受 `max_batch_size` 与服务端资源限制约束。
- 背压依赖 gRPC 流控与消费端处理速度；服务端必须提供限流和资源限制（§3.5），不得无限缓冲。
- 禁止全量加载：Extension 接收批次后即可写入本地 Parquet，不需要完整数据集驻留内存（§3.3）。
- 取消必须幂等：`Cancel(request_id)` 重复调用结果一致；取消时同时关闭远端 Scanner 与本地文件 Reader（§7.6）。
- 单条消息大小上限、流控窗口、分页与断点续传语义方案未定义，待定（TBD）。

## 7. 版本兼容策略

- 包名带版本 `duckdb.hbase.v1`；破坏性变更必须新增 `v2` 包，不能原地修改 `v1` 语义。
- 字段只增不改：新增字段使用新编号，禁止复用或重排已有编号。
- 删除字段必须 `reserved` 其编号与名称，防止后续误用。
- 不得修改已有字段的类型、编号或语义；需要改语义时新增字段。
- proto3 默认保留未知字段，旧客户端应忽略不识别的新字段。
- 能力差异通过能力协商表达，不能靠“猜测对端实现”来兼容。
- 方案要求避免 C++ 与 Java 两套模型不一致，并为 Adapter SPI 增加版本号（§3.7）。
- 兼容窗口、旧版本支持时长与 SPI 版本号格式待定（TBD）。以上演进规则为草案约定，须经协议评审确认。

## 8. 与 Protobuf 文件的对应关系

| 文档内容 | proto 元素 |
| --- | --- |
| 第 2 节服务与 RPC | `service HBaseDataService` 及其 6 个 `rpc` |
| 3.1 ColumnRef | `message ColumnRef` |
| 3.2 ScanRequest | `message ScanRequest` |
| 3.3 BatchGetRequest | `message BatchGetRequest` |
| 3.4 ArrowBatch | `message ArrowBatch` |
| 3.5 未定义消息 | 空消息占位，仅含待定注释，不含字段 |
| 第 4 节错误模型 | 未编码为 proto 字段，错误码与 `ErrorResponse` 待定（TBD） |
| 第 5 节能力协商 | `GetCapabilities` + `CapabilityResponse`（字段待定） |
| 第 7 节版本兼容 | `package duckdb.hbase.v1`、`java_package` |

字段编号严格按方案 §3.5 的顺序从 1 递增，不跳号、不补造字段。修改 proto 前须先更新本文档与 [gateway-design.md](gateway-design.md)，并走协议评审。
