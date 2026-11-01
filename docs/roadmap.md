# 实施路线图（M1-M5）

> 提炼自 [技术方案 V1.0](../tech_v0.1.md) §9-§11。若与方案冲突，以方案为准。

本路线图把技术方案第十章的五阶段交付展开为可勾选的任务清单。架构与依赖方向见 [architecture.md](architecture.md)，Extension 侧设计见 [extension-design.md](extension-design.md)，缓存实现细节见 [cache-design.md](cache-design.md)，编码与评审约束见 [coding-standards.md](coding-standards.md)。

## 1. 当前状态

- 设计阶段已完成：`tech_v0.1.md`（V1.0，13 章）为唯一设计基线，覆盖架构、模块、Provider 协议、缓存、部署、分包与里程碑。
- 代码与测试基础设施尚未开始：`extension/`、`gateway/`、`protocol/` 无构建系统与可编译产物，测试策略见 [testing.md](testing.md)，自 M1 起逐步落地。
- 已确认的边界：第一阶段只读、不实现 DuckDB Catalog、Extension 只依赖 `RemoteScanProvider`、缓存属于 Extension。
- 已锁定的选型：C++ Extension、Java Gateway、gRPC + Protobuf + Arrow IPC、Parquet 缓存 + SQLite 元数据、CMake + Maven 分离构建。

## 2. 里程碑总览

| 阶段 | 目标 | 核心交付 | 验收标准 | 依赖 |
| --- | --- | --- | --- | --- |
| M1 | 打通最小可用链路 | DuckDB C++ Extension、Java Gateway、Native HBase Adapter、`hbase_scan()`、Arrow IPC Streaming、Python 与 CLI 示例 | 能执行真实 HBase Scan | 无（仅依赖设计基线） |
| M2 | 接口解耦 | `IRemoteScanProvider`、Gateway SPI、Provider Capabilities、gRPC 协议、Native Java Adapter 独立模块 | Extension 不依赖 Native Adapter | M1 |
| M3 | 缓存闭环 | Segment、Coverage Index、Parquet、Metadata、Full Hit、TTL、缓存统计 | 重复查询不访问 HBase | M2（稳定的 Provider 协议） |
| M4 | 增量补列 | Partial Hit、Batch Get、RowKey Merger、Copy-on-Write、并发请求合并 | 增加列可以按 RowKey 补齐 | M3（Segment 与元数据） |
| M5 | 生产级扩展 | REST / Thrift / Phoenix Adapter、Kerberos、TLS、缓存淘汰、监控与审计、多平台构建与发行 | 支持多种接入方式、并发与失效 | M4 |

## 3. M1 基础闭环

范围：从 SQL 到 HBase 的单向读取链路，单 Adapter、无缓存、串行执行。

关键任务：

- [x] 锁定 DuckDB 版本与源码 Commit（v2.0.0 / d54cbbe），基于官方 Extension Template 搭建 CMake 工程与 SQLLogicTest 骨架
- [x] 实现 Extension Registry 与 `hbase_connect()` / `hbase_disconnect()`（含端点解析、会话级连接表、fallible 声明）
- [x] `hbase_scan()` 参数解析与校验
- [x] `IRemoteScanProvider` + gRPC 客户端（Health / GetCapabilities / GetSchema），`hbase_connect` 建连即握手
- [x] `hbase_scan()` 数据面：Arrow IPC 解码、后台流式拉取与 DataChunk 填充（UTF8 列）
- [ ] 列类型映射（BIGINT/DOUBLE/TIMESTAMP）与多版本展开
- [ ] 实现 `hbase_scan()` Table Function 的 Bind、Init、Global/Local State 与并行 Scan 骨架
- [ ] 实现 Projection Pushdown 与 Filter Pushdown 的最小可用路径（Required Columns 与 Output Columns 分离）
- [ ] 实现 `hbase_columns()` / `hbase_tables()` 的 Schema 获取
- [x] 搭建 Java Gateway（Maven 多模块）与 gRPC 服务端骨架（Health/Capabilities/Schema/Cancel 已接通）
- [ ] 实现 Native HBase Adapter 的 `scan()`，使用 HBase Java Client 2.x
- [ ] 定义并实现 Arrow IPC Streaming 的批次编码与解码
- [ ] 实现 `DataChunkConverter`（Arrow → DuckDB DataChunk）
- [ ] 提供 CLI 与 Python 使用示例，并在本地 HBase 2.x 上跑通

退出标准（DoD）：

- [ ] `SELECT * FROM hbase_scan('prod', 'default', '<table>')` 在真实 HBase 2.x 上返回正确行数与列
- [ ] 投影与过滤下推有单测覆盖，过滤列未被遗漏
- [ ] 结果集以流式批次返回，内存占用不随结果集线性增长
- [ ] CLI 与 Python 示例可复现，步骤写入 `examples/` 与用户文档
- [ ] 未实现项（缓存、多 Adapter）在文档中明确标注

风险与缓解：

- DuckDB C++ Extension API 非稳定 ABI：锁定版本与 Commit，升级作为独立任务评估，不在功能开发中跟随主干。
- Arrow IPC 与 DuckDB 类型不完全对齐：先实现白名单类型映射，其余类型显式报错，不静默转换。
- 真实 HBase 环境获取困难：优先用单机 HBase 2.x 或容器化环境，环境方案待定（TBD）。

## 4. M2 接口解耦

范围：把 Extension 与具体 Adapter 彻底切开，协议与能力协商定型。

关键任务：

- [ ] 定义 `IRemoteScanProvider` 接口（Scan、BatchGet、GetSchema、Cancel、CheckHealth、GetCapabilities）
- [ ] 定义 `ProviderCapabilities` 结构并实现能力协商
- [ ] 用 Protobuf 定义 ScanRequest / ScanResponse / ArrowBatch / SchemaRequest / BatchGetRequest / 错误模型
- [ ] 实现 gRPC 客户端与服务端，替换 M1 的直连路径
- [ ] 定义 Gateway SPI `HBaseAccessAdapter`（capabilities、getSchema、scan、batchGet、cancel、health）
- [ ] 将 Native Java Adapter 拆为独立 Maven 模块，`gateway-server` 仅通过 Factory 装配
- [ ] 实现远端错误到本地错误的映射与重试策略（Schema 查询、Scan 建立可重试；流读取谨慎），并补充协议兼容性与错误映射单测

退出标准（DoD）：

- [ ] `scan` 模块只依赖 `remote_scan_provider.hpp`，代码层面无 Adapter 引用
- [ ] 新增或替换 Adapter 不需要改动 Extension 代码
- [ ] 能力协商用例覆盖"不支持 Batch Get""不支持时间戳"等组合
- [ ] 协议字段与错误码在 `docs/protocol.md` 中定稿
- [ ] M1 的 SQL 用例在 gRPC 路径下全部通过

风险与缓解：

- C++ 与 Java 两套模型漂移：协议以 Protobuf 为唯一来源，双方生成代码，禁止手写重复 DTO。
- 能力协商过度设计：只覆盖方案 §3.4 列出的能力位，不做推测性扩展。
- 协议早期变动成本高：先定 Scan 语义，BatchGet 在 M4 前只做占位定义。

## 5. M3 缓存闭环

范围：Full Hit 路径、Segment 生命周期、元数据与统计。

关键任务：

- [ ] 定义 `ScanPlan`、`CacheLookupResult`、`SegmentDescriptor` 等核心数据结构
- [ ] 实现 Segment 划分与 `CacheSegmentManager`
- [ ] 实现 `CacheCoverageIndex`：范围、列、时间戳语义、TTL 的覆盖判定
- [ ] 实现 `ParquetCacheStorage`（CreateWriter / OpenReader / DeleteSegment）
- [ ] 实现 SQLite 元数据表（`cache_segment`、`cache_segment_column`）与 staging + 原子 Metadata Commit
- [ ] 实现 Full Hit 读取路径，命中时完全不访问 Gateway，且 LIMIT 不影响缓存完整性
- [ ] 实现 TTL 与 `CacheInvalidationManager` 的过期失效
- [ ] 实现 `hbase_cache_stats()` / `hbase_cache_clear()` 与相关 `SET` 配置
- [ ] 补充覆盖索引、存储损坏、原子提交的单测

退出标准（DoD）：

- [ ] 相同查询第二次执行时远端 Scan 次数为 0
- [ ] 覆盖判定按 HBase 字节序比较，边界用例（相邻、重叠、包含、单字节缺口）通过
- [ ] 违反完整命中五项条件时不会误判为 Full Hit
- [ ] 进程崩溃后元数据可恢复，未提交 Segment 不可见
- [ ] 缓存命中率、远端 Scan 次数、内存占用可被观测

风险与缓解：

- 覆盖索引算法复杂（方案列为待解决实现问题之一）：先实现单 Segment 单范围，再多 Segment 多范围，逐步加复杂度。
- Parquet 与元数据不一致：以 staging + 原子提交为唯一写入路径，禁止旁路写文件。
- 缓存误命中导致数据错误：命中判定失败时一律回退 Cache Miss。

## 6. M4 增量补列

范围：Partial Hit、Batch Get 补列、行级合并与并发控制。

关键任务：

- [ ] 实现 Partial Hit 判定与缺口列计算
- [ ] 实现 Provider `BatchGet` 与 Gateway 侧批量读取
- [ ] 实现 `CacheMerger` 的 RowKey 对齐与 NULL 语义
- [ ] 区分"列存在且值为空""列不存在""RowKey 不存在""查询期间 RowKey 变化"
- [ ] Batch Get 未返回某 RowKey 时不写入负结果缓存
- [ ] 实现 Copy-on-Write 合并与原子提交，兼容 Gateway 不支持 Batch Get 时退化为范围 Scan
- [ ] 实现并发控制：Segment Lock、Column Lock、SingleFlight、Reader Reference Count、Generation CAS
- [ ] 补充合并、并发、失败恢复（INIT/WRITING/VALIDATING/COMMITTED）用例

退出标准（DoD）：

- [ ] 增加列后再次查询触发补列，行数不变且新旧列值均正确
- [ ] 同一列的并发补列请求被合并为一次远端调用
- [ ] 并发提交不出现丢失更新或半成品 Segment
- [ ] 四阶段故障注入均按方案规定处理，已提交数据不丢失
- [ ] 取消查询时远端 Scanner 与本地 Reader 同时关闭

风险与缓解：

- 行对齐错误导致串列：以 RowKey 为唯一关联键，禁止按行号或位置拼接。
- 并发写覆盖：统一走 Generation CAS，冲突时重试或放弃，不就地修改 Segment。
- 补列放大远端压力：SingleFlight 合并同列请求，必要时限制并发补列数。

## 7. M5 生产级扩展

范围：多 Adapter、安全、淘汰、可观测性与发行。

关键任务：

- [ ] 实现 REST Adapter，并验证其能力差异（批量 Get 支持情况待定，TBD）
- [ ] 评估并实现 Thrift Adapter（是否纳入首批待定，TBD）
- [ ] 实现 Phoenix Adapter，独立处理 SQL 语义与 Cell Scan 语义的差异
- [ ] 实现 Kerberos 认证与 TLS
- [ ] 实现 Gateway API 认证、表级权限控制与审计日志
- [ ] 实现缓存淘汰策略（容量上限、LRU 或等价策略）
- [ ] 补充监控指标导出与查询追踪（Request ID）
- [ ] 多平台构建与发行：构建矩阵、产物打包、安装文档
- [ ] 完善集成环境矩阵与性能基线归档

退出标准（DoD）：

- [ ] 至少两种 Adapter 通过同一套 SQL 用例
- [ ] 开启 Kerberos 与 TLS 后链路可用，凭据不出现在 SQL 明文与日志中
- [ ] 缓存达到容量上限时按策略淘汰且不影响正确性
- [ ] 监控与审计字段可被外部采集
- [ ] 发行产物在支持平台上可安装并跑通示例

风险与缓解：

- Phoenix 语义混用：单独成组测试，不复用 Native Adapter 的预期结果。
- 安全改造影响既有链路：安全能力默认可关闭，分环境验证。
- 淘汰策略引入并发缺陷：淘汰与读取的竞态纳入并发测试用例。

## 8. 里程碑依赖关系

```text
M1 基础闭环（hbase_scan 通路）
  └──> M2 接口解耦（Provider 协议定型）
         └──> M3 缓存闭环（Segment + Coverage Index + Full Hit）
                └──> M4 增量补列（Batch Get + Merger + 并发）
                       └──> M5 生产级扩展（多 Adapter + 安全 + 淘汰）
```

- M2 依赖 M1 已跑通的 `hbase_scan()` 通路：没有可执行的 Scan，协议无法验证。
- M3 依赖 M2 的稳定 Provider 协议：缓存写入的数据必须来自统一协议，而非某个 Adapter 的私有格式。
- M4 依赖 M3 的 Segment 与元数据：补列是基于既有 Segment 的增量行为。
- M5 依赖 M4 的并发与失效机制：多 Adapter 与淘汰都必须建立在正确的并发控制之上。

## 9. 后续待解决问题（方案 §13，均为对应里程碑的前置输入）

1. DuckDB Extension 的 Scan API 实现：Table Function 的 Projection Pushdown、Filter Pushdown、Global/Local State 与并行 Scan 的具体实现（阻塞 M1）。
2. Provider Protocol 的正式定义：Protobuf、Arrow IPC、错误模型、能力协商与 Gateway Adapter SPI，避免 C++ 与 Java 两套模型不一致（阻塞 M2）。
3. Cache Coverage Index 的算法：多 Segment、多列、多 RowKey 范围之间的覆盖关系（阻塞 M3）。

## 10. 版本与文档策略

- `tech_v0.1.md` 是设计基线，任何实现偏离必须先记录理由，不得反过来修改既有结论以迁就代码。
- 每完成一个里程碑，更新本文件的任务勾选、退出标准与状态，并在 `CHANGELOG.md` 记录。
- 接口、配置、错误码变更同 PR 更新 `docs/` 对应文档（见 AGENTS.md 文档同步矩阵）。
- 里程碑验收指标中凡方案未给出的数值，一律标注"待定（TBD）"，由性能测试基线确定后再补。
- 版本号在 M1 交付前保持 `v0.1` 设计态，首个可用版本号规则待定（TBD）。
