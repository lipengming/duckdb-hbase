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
- [ ] 列类型映射（BIGINT/DOUBLE/TIMESTAMP）
- [x] 实现 `hbase_scan()` Table Function 的 Bind、Init、Global State 与流式 Scan（并行扫描待 M2）
- [ ] 实现 Projection Pushdown 与 Filter Pushdown 的最小可用路径（Required Columns 与 Output Columns 分离）
- [ ] 实现 `hbase_columns()` / `hbase_tables()` 的 Schema 获取
- [x] 搭建 Java Gateway（Maven 多模块）与 gRPC 服务端骨架（Health/Capabilities/Schema/Cancel 已接通）
- [x] 实现 Native HBase Adapter 的 `scan()` / `batchGet()`，使用 HBase Java Client 2.x
- [x] 定义并实现流式批次编码与解码（M1 用 UTF8 列式载荷；Arrow IPC 切换见技术债）
- [x] 实现 `DataChunkConverter`（列式载荷 → DuckDB DataChunk）
- [x] 提供 CLI 使用示例（`docs/quickstart.md`），并在本地 HBase 2.6.6 上跑通

退出标准（DoD）：

- [x] 显式列扫描在真实 HBase 2.6.6 上返回正确行数与列（`SELECT *` 需 M3 前的 Schema 能力）
- [ ] 投影与过滤下推有单测覆盖，过滤列未被遗漏
- [x] 结果集以流式批次返回（后台线程 + 有界队列），内存占用不随结果集线性增长
- [ ] CLI 与 Python 示例可复现，步骤写入 `examples/` 与用户文档
- [x] 未实现项（缓存、多 Adapter、类型映射、多版本）在文档与运行时错误中明确标注

风险与缓解：

- DuckDB C++ Extension API 非稳定 ABI：锁定版本与 Commit，升级作为独立任务评估，不在功能开发中跟随主干。
- Arrow IPC 与 DuckDB 类型不完全对齐：先实现白名单类型映射，其余类型显式报错，不静默转换。
- 真实 HBase 环境获取困难：优先用单机 HBase 2.x 或容器化环境，环境方案待定（TBD）。

## 4. M2 接口解耦

范围：把 Extension 与具体 Adapter 彻底切开，协议与能力协商定型。

关键任务：

- [x] 定义 `IRemoteScanProvider` 接口（Scan、BatchGet、GetSchema、HealthCheck、GetCapabilities；Cancel 待接入）
- [x] 定义 `ProviderCapabilities` 结构并实现能力协商
- [x] 用 Protobuf 定义请求/响应与错误模型（`ErrorInfo` + `ErrorCode` + trailing metadata）
- [ ] 实现 gRPC 客户端与服务端，替换 M1 的直连路径
- [x] 定义 Gateway SPI `HBaseAccessAdapter`（capabilities、getSchema、scan、batchGet、cancel、health）
- [x] 将 Native Java Adapter 拆为独立 Maven 模块，`gateway-server` 仅通过 Factory(ServiceLoader) 装配
- [x] 实现远端错误到本地错误映射与重试（幂等 RPC 重试一次；Scan 出数据后不重试），补充协议兼容性与结构化错误单测

退出标准（DoD）：

- [x] `scan` 模块只依赖 `remote_scan_provider.hpp`，代码层面无 Adapter 引用
- [ ] 新增或替换 Adapter 不需要改动 Extension 代码
- [x] 能力协商用例覆盖（`supports_scan` 缺失时 Bind 直接报错；Capabilities 字段稳定性测试）
- [x] 协议字段与错误码在 `docs/protocol.md` 中定稿
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

## 10. 已知缺陷与技术债

按影响面排序，均需在对应里程碑内闭环：

| # | 问题 | 影响 | 归属 |
| --- | --- | --- | --- |
| 1 | **上游 DuckDB 2.0 缺陷**：扩展表函数输出上的 `FILTER (IS NULL)` / `DISTINCT (IS NULL)` 误判（`count(col)`、`WHERE col IS NULL` 均正确） | 直接聚合 NULL 结果不可信 | 待上游修复 |
| 1.1 | 已最小复现：注册一个只返回常量列的测试表函数——单列 99 行时 `FILTER (IS NULL)` 报 37；两列时第一列报 37、第二列报 0；同样数据用内置 `read_csv` 完全正确 | 说明缺陷位于 DuckDB 与扩展表函数的交互层，不在 Extension 数据/validity 写入 | 附件：复现代码见下 |
| 2 | HBase 按列扫描会跳过“没有任何匹配 cell”的行，NULL 不可观测 | 只查缺失列时行数偏少 | M1 收尾（自动附加定位列或文档约定） |
| 3 | 数据面用自定义 UTF8 列式编码，未落地 Arrow IPC | 与设计基线的 `ArrowBatch.ipc_payload` 语义不完全一致 | M2 |
| 4 | 列类型全部为 `VARCHAR`，无 BIGINT/DOUBLE/TIMESTAMP 映射 | 用户必须手写 CAST，聚合易错 | M1 收尾 |
| 5 | 多版本未展开（`max_versions > 1` 报错） | 无法做版本对比 | M2 |
| 6 | `hbase_connect` 只支持端点字符串，无 JSON 配置 / 超时 / TLS / 认证 | 生产可用性不足 | M2/M5 |
| 7 | 单线程扫描、无 Filter Pushdown、无 RowKey 区间切分 | 大范围查询吞吐受限 | M2 |
| 8 | 无 C++ 单元测试（仅 SQLLogicTest + Java 集成测试） | 覆盖索引/合并等纯逻辑难回归 | M3 前置 |
| 9 | 扩展未签名、未发布到 Extension Repository | 用户必须 `LOAD` 本地文件 + `-unsigned` | M5 |
| 10 | CI 中 vcpkg 编译 gRPC 耗时 30-60 分钟 | 反馈慢 | 持续优化（缓存/预编译） |

### 缺陷 #1 最小复现（供上游 issue 使用）

```cpp
// 注册：TableFunction t("dbg", {BIGINT}, Execute, Bind);
// Bind: 返回 1 列 VARCHAR/BIGINT；Execute: 写 n 行常量、全部有效
SELECT count(*) c, count(v) nn, count(*) FILTER (WHERE v IS NULL) a FROM dbg(99);
-- 期望 (99, 99, 0)，实际 (99, 99, 37)
```

对照：`SELECT count(*) FILTER (WHERE phone IS NULL) FROM read_csv(...)` 结果正确；`CREATE TABLE t AS SELECT ...` 后再聚合结果正确。
规避方式：直接对表函数结果做 `FILTER (IS NULL)` 前先物化。

## 11. 版本与文档策略

- `tech_v0.1.md` 是设计基线，任何实现偏离必须先记录理由，不得反过来修改既有结论以迁就代码。
- 每完成一个里程碑，更新本文件的任务勾选、退出标准与状态，并在 `CHANGELOG.md` 记录。
- 接口、配置、错误码变更同 PR 更新 `docs/` 对应文档（见 AGENTS.md 文档同步矩阵）。
- 里程碑验收指标中凡方案未给出的数值，一律标注"待定（TBD）"，由性能测试基线确定后再补。
- 版本号在 M1 交付前保持 `v0.1` 设计态，首个可用版本号规则待定（TBD）。
