# 缓存引擎设计

> 提炼自 [技术方案 V1.0](../tech_v0.1.md) §2.6-2.7、§7.3-7.6。若与方案冲突，以方案为准。

本文描述 Extension 内部的列式 Scan Cache。分层与依赖关系见 [architecture.md](architecture.md)，类与接口设计见 [extension-design.md](extension-design.md)，SQL 用法见 [user-guide.md](user-guide.md)。

## 1. 定位与不变量

缓存是 Extension 的核心能力，不属于 Gateway。方案要求 Cache 独立于 HBase 与 Gateway：它不认识 gRPC、REST、Thrift、Phoenix，不持有任何 Gateway 连接；唯一的外部依赖是补列时调用的 `IRemoteScanProvider::BatchGet()`，Provider 是 Extension 与 Gateway 之间唯一的依赖边界。

| 编号 | 不变量 |
| --- | --- |
| I1 | 缓存属于单个 DuckDB 进程本地，默认不跨进程、不跨分析节点共享 |
| I2 | Segment 是缓存基本管理单元，不能以 SQL 文本 Hash 作为缓存 Key |
| I3 | 覆盖判断以 RequiredColumns（Output ∪ Filter 列）为单位，不以 SELECT 列为单位 |
| I4 | 补齐与合并以 RowKey 为关联键，按 HBase 字节序比较 |
| I5 | 范围、列、时间戳语义、未过期、数据完整五项全满足才算 Full Hit |
| I6 | 第一版禁止 LIMIT 影响缓存数据完整性，不得把截断结果当作整段范围的缓存 |
| I7 | 补列采用 Copy-on-Write，新 Segment 由原子 Metadata Commit 生效 |
| I8 | 写失败只回滚到未提交状态；已提交 Segment 不回滚 |

## 2. 核心概念

| 概念 | 含义 | 关键字段 / 接口 |
| --- | --- | --- |
| Segment | 覆盖某 RowKey 区间与某列集合的列式文件，是缓存基本单元 | `segment_id`、`start_row`、`stop_row`、`row_count`、`file_path` |
| Coverage Index | 记录哪些 RowKey 范围、哪些列已被缓存的索引 | `CacheCoverageIndex` 维护，`CacheLookupEngine` 查询 |
| Segment Manifest | 描述 Segment 的清单；方案中仅以源文件 `segment_manifest.cpp` 出现 | M3 未单独落盘：列清单与一致性语义存于 `cache_segment` / `cache_segment_column`，启动时由元数据重建内存视图（结论见 §7.2） |
| Metadata | 独立小型本地数据库（SQLite）中的段元数据 | 表 `cache_segment`、`cache_segment_column` |
| RowKey Range | 二进制区间，边界语义与 HBase 一致 | `RowKey` = `vector<uint8_t>`；`RowKeyRange{start, stop, include_start, include_stop}` |
| Column Set | 按 `family:qualifier` 归一的列集合，分已缓存与缺失两种视图 | `available_columns`、`missing_columns`、`logical_type` |
| TTL | Segment 过期时间，取自 `hbase_cache_ttl`（示例值 300 秒） | `expires_at` |
| Generation | 段版本号，用于提交时防止并发覆盖 | `generation`、Generation CAS |

来源中只出现 Generation，没有独立的 Epoch 概念；若后续需要表级或库级版本，需额外定义，现按待定处理。

## 3. 分层结构

```
CacheManager (ICacheManager)
    +-- CacheLookupEngine -------- 覆盖判断，产出 CacheLookupResult
    |      +-- Coverage Index / Segment Index
    +-- CacheSegmentManager ------ Segment 生命周期（Reader / Writer）
    +-- CacheMerger -------------- 按 RowKey 合并与补列
    +-- CacheInvalidationManager - TTL / 手动失效 / CDC
    +-- CacheEvictionManager ----- 容量淘汰
    +-- ICacheStorage ------------ L2 Parquet / L1 Memory / Metadata(SQLite)
```

| 组件 | 类 | 职责 |
| --- | --- | --- |
| 门面 | `CacheManager` | 对外暴露 Lookup / OpenReader / BeginWrite / Invalidate / GetStatistics，是 scan 层唯一入口 |
| 覆盖判断 | `CacheLookupEngine` | 把 `ScanPlan` 翻译为分段覆盖计划，不读数据 |
| 覆盖索引 | `CacheCoverageIndex` | 维护并查询 Segment 覆盖关系，见 §5 |
| 段管理 | `CacheSegmentManager` | 创建、打开、删除 Segment，配合 Segment Lock 与引用计数 |
| 合并 | `CacheMerger` | 按 RowKey 对齐缓存列与补齐列 |
| 失效 | `CacheInvalidationManager` | TTL 过期、手动失效、后续 CDC 失效 |
| 淘汰 | `CacheEvictionManager` | 容量超限时回收 Segment |
| 存储 | `ParquetCacheStorage` / `MemoryCacheStorage` | 统一 `ICacheStorage`：OpenReader / CreateWriter / DeleteSegment |
| 元数据 | SQLite `MetadataStore` | 段与列的权威索引，与 Parquet 通过 staging + 原子提交衔接 |
| 分区 | L1 Memory / L2 Parquet / Metadata | L1 与 L2 的提升、降级触发条件待定（TBD） |

## 4. 查询命中判定

| 结果 | 判定条件 | 执行分支 |
| --- | --- | --- |
| Full Hit | 请求范围完整覆盖、RequiredColumns 全覆盖、时间戳语义兼容、未过期、数据完整性校验通过 | `OpenReader` 直接读 Segment，不访问 HBase |
| Partial Hit | 有覆盖但存在范围缺口（`uncached_ranges` 非空）或某段缺列（`missing_columns` 非空） | 命中部分读缓存；缺口走 Provider Scan 写新段；缺列走 BatchGet 补列 |
| Cache Miss | 无可用覆盖，或命中段全部不可用（过期、版本不兼容、校验失败、开关关闭） | 全量走 Provider Scan，成功后 `BeginWrite` 落盘 |

```
ScanPlan{table, rowkey_ranges, required_columns, consistency}
        |
        v
[0] 全局开关/本次 cache := false ? ---------> Miss（直连 Provider，不写缓存）
        |
        v
[1] CacheLookupEngine::Lookup(plan) --> [2] CoverageIndex 求每段范围的 Segment 交集
        |
        +-- 交集为空 -----------------------------> Miss
        +-- 存在未覆盖子区间 ---------------------> Partial（uncached_ranges）
        +-- 某段缺列 -----------------------------> Partial（missing_columns）
        v
[3] 逐段过滤：未过期 & ScanConsistency 兼容 & 完整性校验通过
        |
        +-- 全部通过且无缺口无缺列 --------------> Full Hit
        +-- 部分通过 ----------------------------> Partial Hit
        +-- 全部失效 ----------------------------> Miss（并触发失效清理）
```

明确不可复用的情形：`max_versions` / `latest_only` 不同（最新版本与历史版本不能共用 Segment）；`timestamp_min` / `timestamp_max` 不兼容；已过期；完整性校验失败（视为失效，不得降级为部分使用）；Segment 尚未 COMMITTED（写入未完成，不得作为命中来源）；带 `LIMIT` 的截断结果。

## 5. Coverage Index 的覆盖判断

Lookup 不能只返回三分类枚举。方案给出的分段覆盖计划：

```
SegmentCoverage { segment_id, range, available_columns, missing_columns }
CacheLookupResult { status, segments[], uncached_ranges[] }
```

原因是"全局列集合"会判错：

```
Segment A: [0000,1000) 列 name, age
Segment B: [1000,2000) 列 name, city
查询:      SELECT name, age, city     --> A 段缺 city，B 段缺 age
```

即"不同范围缺不同列"，必须按范围分段表达覆盖。判定步骤：

1. 归一化输入：列以 `(family, qualifier)` 为单位，范围按字节序比较，与 HBase 一致。
2. 端点切分：取请求范围端点与所有相交 Segment 端点，切成互不重叠的原子子区间。
3. 逐子区间求交：收集相交 Segment，各段列集合并集即该子区间已覆盖列，其余为缺失列。
4. 无任何 Segment 覆盖的原子子区间进入 `uncached_ranges`。
5. 归并列集合相同且相邻的子区间，再产出 `CacheLookupResult`。

| 已知难点 | 待定内容 |
| --- | --- |
| 多 Segment 重叠 | 同一子区间被多段覆盖时按 `generation` 高者还是 `created_at` 新者取用，是否需读时去重 |
| Segment 切分粒度 | 按 HBase Region 边界、固定 RowKey 区间，还是按 Scan 请求范围切分 |
| 边界语义 | `include_start` / `include_stop` 与 API 左闭右开的统一约定，边界相等时的碰撞规则 |
| 整族请求 | 请求只给 family 时，如何折算为列级 Segment 的覆盖 |
| 缺列与缺范围并存 | 合并优先级，避免同一 RowKey 被 Scan 与 BatchGet 重复拉取 |
| 索引形态 | 遍历 SQLite 区间查询，还是内存有序区间树 |

## 6. 部分命中补列

```
Cache Segment
     |
     v
读取 RowKey + 已缓存列 --> 计算缺失列 --> Provider BatchGet
                                              |
     +----------------------------------------+
     v
按 RowKey 对齐（RowKey Merger）--> 补充 NULL / 实际 Cell
     |
     v
生成新 Segment（Copy-on-Write）--> 原子 Metadata Commit（Generation CAS）
```

必须区分的四种情况：列存在且值为空（按真实空值写入，与"列不存在"分开表达）；列不存在（按 HBase 语义映射为 NULL）；RowKey 不存在；RowKey 在查询期间发生变化（按版本语义判定是否沿用）。

关键约束：Cell 不存在应映射为 NULL，不能误判为 RowKey 不存在；Batch Get 未返回某 RowKey 时不能简单当作完整负结果缓存，可能是并发删除、权限过滤或数据变化。旧 Segment 不原地修改，新段提交后由失效/淘汰回收，读者通过引用计数保证文件不被提前删除。

## 7. 物理存储格式

方案确定的信息只有：根目录由 `hbase_cache_directory` 配置（示例 `./hbase_cache`），模式由 `hbase_cache_mode = 'parquet'` 选择，数据写 Parquet、元数据写 SQLite，两者通过 staging + 原子 Metadata Commit 保证可恢复性。

```
<hbase_cache_directory>/          # 例：./hbase_cache
    ├── <staging 临时文件>        # WRITING 阶段，失败即删除
    ├── <已提交 Segment 文件>     # 由 cache_segment.file_path 指向
    └── <metadata 数据库文件>     # SQLite
```

### 7.1 M3 落地布局（定案）

```
<hbase_cache_directory>/
├── metadata.db                 # SQLite：cache_segment / cache_segment_column（WAL + synchronous=FULL）
├── segments/<segment_id>.seg   # 已提交段数据文件
└── staging/<segment_id>.tmp    # 写入中；Finish 时 rename 到 segments/，崩溃残留由 CleanupStaging 清理
```

- 段 ID 由"毫秒时间戳 + 随机位 + 进程内自增"构成，跨进程/同毫秒唯一，因此无需按表分目录。
- 崩溃恢复：`staging/*` 全部视为半成品删除；`cache_segment` 中非 `COMMITTED` 的记录连同文件一并清理。
- `hbase_cache_clear` 按元数据逐个删除文件，不依赖路径前缀。

段文件格式（`DHBSEG01`，见 `storage/segment_file_storage.cpp` 头注释）：前置 magic、若干块（`u32 rows | u32 payload_size | payload | u32 crc32`）、footer（版本 / block_rows / 总行数 / 块数 / 列清单 + CRC32 + 长度 + magic）。payload 逐列写 validity 位图、长度表与拼接数据。所有整数小端，行数、长度表、数据长度三者不一致即判定损坏。

> 与方案 §2.7 的 Parquet 差异及理由见 `tech_v0.1.md` 附录 A ADR-002。

`cache_segment` 字段：`segment_id`、`table_id`、`start_row` / `stop_row`（BLOB）、`row_count`、`created_at`、`expires_at`、`generation`、`file_path`、`status`。`cache_segment_column` 字段：`segment_id`、`family`、`qualifier`、`logical_type`，主键 `(segment_id, family, qualifier)`。`status` 取值与 §8 失败恢复四态的映射待定。

Segment Manifest 在方案中仅有文件名，无字段定义。它是独立落盘清单还是从上述两表派生的内存视图，属待定（TBD）；这决定启动时重建 Coverage Index 的路径。

| 完整性校验项 | 方案状态 |
| --- | --- |
| 行数 | 元数据有 `row_count`，可在 VALIDATING 阶段与 Parquet 实际行数比对 |
| 文件存在且可读 | 打开失败即校验失败，按 VALIDATING 失败 Abort |
| 文件大小 | 未定义是否入库与校验，待定（TBD） |
| 校验和 | 未指定算法与存放位置，待定（TBD） |

后两项定案需要：摘要算法选择、校验和写进 `cache_segment` 还是独立 manifest、校验失败时删除文件还是仅标记失效。

## 8. 一致性策略

**数据版本语义**：`ScanConsistency{min_timestamp, max_timestamp, max_versions, latest_only}` 是可复用性的判定维度。版本语义不兼容时不得 Full Hit，须回源。Segment 侧如何记录快照（全量序列化该结构，还是只记时间范围与 `max_versions`）待定（TBD）。

**TTL 过期**：`hbase_cache_ttl` 决定 `expires_at`；过期段不得参与覆盖计算，也不得被 OpenReader 打开。清理时机（惰性还是后台扫描）待定。

**显式失效**：`Invalidate(table, optional<RowKeyRange>)` 覆盖整库、单表、范围三级；失效只改元数据可见性，被引用文件等引用归零后删除。CDC 失效属后续能力，第一版不实现。必须区分"缓存覆盖完整性"与"数据新鲜度"：前者保证列与范围不缺，后者由 TTL / 手动失效 / CDC 保证，列覆盖完整不代表数据新鲜。

**并发查询与并发写**：`Segment Lock`、`Column Lock`、`SingleFlight`、`Reader Reference Count`、`Generation CAS`。方案示例中，Query A 与 B 同缺 `age` 时合并为一次补列请求，Query C 的 `city` 独立请求；提交阶段用 Generation CAS 防止并发写互相覆盖，失败方放弃本次写入并重试或读旧段，不得覆盖他人结果。

**失败恢复**：

```
INIT --> WRITING --> VALIDATING --> COMMITTED
```

| 阶段 | 失败处理 |
| --- | --- |
| INIT | 直接终止，不产生文件 |
| WRITING | 删除临时文件，半写文件不得进入元数据 |
| VALIDATING | 校验失败则 Abort 并删除文件 |
| COMMITTED | 元数据已生效，不回滚已发布 Segment |

查询取消时必须同时关闭远端 Scanner 与本地文件 Reader，避免句柄泄漏导致文件无法淘汰。崩溃恢复依赖 staging 约定：未在 `cache_segment` 中登记的文件应按临时文件清理（staging + 原子提交的直接推论），具体判定规则依赖 §7 的命名约定，待定。

## 9. 统计与可观测指标

`ICacheManager::GetStatistics()` 对应 SQL 的 `hbase_cache_stats()`；输出列已在 §9.1 定稿（方案原文未给出，属 M3 收敛项）。可引用的指标命名来自性能测试章节：

| 指标 | 归属 | 说明 |
| --- | --- | --- |
| Cache Hit Rate | 缓存 | 命中率，Full/Partial 是否分开统计待定 |
| HBase Scan Count | 远端 | 远端 Scan 次数，反映缓存对远端的削减 |
| HBase Get Count | 远端 | 补列 BatchGet 次数 |
| Memory Usage | 缓存 | 内存占用，含 L1 与元数据开销 |
| Cache Write Throughput | 缓存 | 缓存写入吞吐 |
| Query Latency | 端到端 | 查询延迟 |
| Gateway Throughput | Gateway | 不属缓存引擎指标 |

`hbase_cache_stats()` 至少应能回答：各表 Segment 数、覆盖的 RowKey 区间与列、总字节数、过期与失效段数；具体列与类型待与 [user-guide.md](user-guide.md) 的输出示例对齐后固定。

### 9.1 `hbase_cache_stats()` 输出列（M3 定稿）

| 列 | 类型 | 含义 |
| --- | --- | --- |
| `namespace` | VARCHAR | HBase namespace |
| `table_name` | VARCHAR | 表名 |
| `segments` | BIGINT | 未过期的 COMMITTED 段数 |
| `rows` | BIGINT | 上述段的行数合计 |
| `bytes` | BIGINT | 上述段的段文件字节数合计 |
| `expired_segments` | BIGINT | 已过期但尚未物理回收的段数 |
| `full_hit_count` | BIGINT | 本会话 Full Hit 次数 |
| `partial_hit_count` | BIGINT | 本会话 Partial Hit 次数（M3 恒为 0） |
| `miss_count` | BIGINT | 本会话 Cache Miss 次数 |
| `remote_scan_count` | BIGINT | 本会话远端 Scan 次数 |
| `remote_get_count` | BIGINT | 本会话远端 BatchGet 次数（M3 恒为 0） |
| `write_error_count` | BIGINT | 本会话缓存写入失败次数（查询结果不受影响） |

统计按表一行；跨表求和即全局口径（集成测试按列名关键字求和断言，列名保持不变）。

## 10. 容量管理与淘汰

- 容量上限由 `hbase_cache_max_size` 配置（示例 `'10GB'`）。
- 淘汰由 `CacheEvictionManager`（`cache_eviction.cpp`）负责；方案将其列入 M5，第一版可不实现自动淘汰。
- 可用输入是元数据字段：`expires_at`（过期优先回收）、`created_at`、`row_count`、`file_path` 对应文件大小（是否入库待定）。
- 淘汰与读者冲突由 Reader 引用计数解决：被引用段只标记待删，引用归零后删除。
- 具体算法（按过期时间、创建时间还是访问热度）方案未指定，属待定（TBD）。按热度淘汰需新增最近访问时间或访问计数，现有元数据表无该字段。

## 11. 与 SQL API 的对应关系

| SQL | 行为 | 涉及组件 |
| --- | --- | --- |
| `hbase_cache_stats()` | 查看缓存统计（列定义见 §9.1） | `CacheManager::GetStatistics()` |
| `hbase_cache_clear()` | 清理所有缓存 | `CacheManager::InvalidateAll()` |
| `hbase_cache_clear(namespace, table)` | 清理单表缓存 | `CacheManager::InvalidateTable()` |
| `hbase_cache_clear(connection, namespace, table)` | 与方案 §4.2 示例签名一致；缓存属于进程本地，连接名仅用于签名兼容 | `CacheManager::InvalidateTable()` |
| `hbase_scan(..., cache := true)` | 单次查询是否使用缓存（默认 true） | `ScanPlan.allow_cache` |
| `SET hbase_cache_enabled` | 全局开关，优先于单查询参数 | Lookup 第 0 步 |
| `SET hbase_cache_ttl` | 新 Segment 的 TTL（秒，0 表示永不过期） | `expires_at` 计算 |
| `SET hbase_cache_max_size` | 容量上限（**M3 仅登记不执行**，淘汰在 M5） | `CacheEvictionManager`（未实现） |
| `SET hbase_cache_directory` | 缓存根目录 | `SegmentFileStorage` / `CacheMetadataStore` |
| `SET hbase_cache_mode` | `file`（默认）/ `memory`；`parquet` 显式报错（ADR-002） | `ICacheStorage` 工厂 |

函数注册入口为 `HBaseExtension::Load()` 中的 `RegisterCacheFunctions()` 与 `cache::RegisterCacheConfiguration()`，实现文件为 `sql/hbase_cache_functions.cpp`。

## 12. M3 实现记录（2026-11）

本节记录 M3 落地时对 §5-§10 中 "待定（TBD）" 项的收敛结果，未在此列出的仍为待定。

### 12.1 已定案

| 待定项 | M3 结论 | 理由 |
| --- | --- | --- |
| Segment 切分粒度 | **按 Scan 请求范围切分**（并行扫描时按分区切分，分区本身是请求范围的连续子区间） | 数据面当前不返回 RowKey（见 §12.3），无法按真实行边界切分；按请求范围切分可保证"段 ⊆ 请求范围"，读出的行不会越界 |
| 边界语义 | 只缓存 **左闭右开**（`include_stop = false`）的请求；`include_stop = true` 一律绕过缓存 | 半开段无法无损表达闭区间端点 |
| 多 Segment 重叠 | 贪心选"能接上游标且 stop 最大"的段；只使用**范围 ⊆ 请求范围**的段 | 结果确定，且不会读出请求范围之外的行 |
| 部分命中（M3） | 不实现。任何缺口/缺列一律 `MISS`，整段回源 | M3 目标是 Full Hit 闭环；Partial 属 M4 |
| 段列集合 | 必须包含全部 RequiredColumns（按 family/qualifier/logical_type 比较） | 列类型不同不得复用 |
| 时间戳语义 | 只复用 `max_versions = 1` 且未指定 `timestamp_min/max` 的段 | 保守策略，无法证明兼容就回源 |
| 索引形态 | 每次 Lookup 从 SQLite 读取该表全部段的元数据（分段数据量小），在内存里求解覆盖计划 | 避免跨连接/跨进程的索引陈旧；覆盖求解是纯函数，可直接单测 |
| 过期清理时机 | 惰性：过期段不参与覆盖、不被打开；**物理删除**发生在 `hbase_cache_clear` 与 M5 淘汰 | 让 `hbase_cache_stats()` 持续报告过期段数 |
| `status` 取值 | `INIT / WRITING / VALIDATING / COMMITTED / INVALID`（ADR-003） | 损坏段需要保留排障信息但不可复用 |
| 完整性校验 | 打开即整段校验：文件大小与元数据一致 + 文件 magic + footer CRC32 + 逐块 CRC32 + 块数/行数与 footer 一致；读取时再按块校验一遍 | 无外部依赖，可覆盖截断、位翻转与半写。代价是命中路径多一次顺序读，换来"绝不会读到一半才发现损坏" |
| `hbase_cache_stats()` 列 | 见 §9.1 | 与集成测试按列名关键字识别的策略兼容 |

### 12.2 数据流（M3 实际实现）

```
hbase_scan Bind
  └── ScanPlan{range, required_columns, consistency, allow_cache}
        │
InitGlobal
  ├── CacheManager::Lookup → ComputeCacheLookup(纯函数)
  │      ├── FULL_HIT → 逐个 OpenReader（全部成功才走命中路径；任一段损坏则标 INVALID 并整体回源）
  │      └── MISS     → BuildPartitions（并行扫描分区）
  │
Execute（每个线程）
  ├── 命中：ICacheSegmentReader::Next → DataChunk（不访问 Provider，不计数远端 Scan）
  └── 未命中：RemoteScanStream → DecodeColumnarBatch → DataChunk
                └── 同时 tee 到 CacheWriteSession：Append 全列批次
                      ├── 分区正常结束 → Finish（rename 发布 + 单事务提交元数据）
                      └── LIMIT / 取消 / 析构 → Abort（删除 staging，不留半成品段）
```

### 12.3 已知限制与前置条件

- **数据面不返回 RowKey**：`hbase_scan` 的输出与缓存段内都没有 RowKey 列，因此 M3 只能按"请求范围"而不是"真实行范围"判定覆盖，也**无法**按 RowKey 裁剪段内行。M4 的 BatchGet 补列与 RowKey Merger 需要先让 Gateway 在批次中附加 `__rowkey` 列（列为 M4 前置任务，见 `docs/roadmap.md` §10 技术债 #11）。
- **Parquet 未落地**：见 `tech_v0.1.md` 附录 A ADR-002，`hbase_cache_mode='parquet'` 显式报错。
- **无淘汰**：`hbase_cache_max_size` 仅登记，容量淘汰在 M5。
- **无并发写入合并**：Segment Lock / Column Lock / SingleFlight / Generation CAS 属 M4；M3 依赖"段 ID 唯一 + 单事务提交"，并发写同一范围只会产生多条互不覆盖的段（覆盖判定仍正确，只是命中率下降）。
- **损坏段的处理**：命中段在 `InitGlobal` 打开阶段被判定损坏时，整体回源（不会读到一半失败），该段标记 `INVALID` 并删除数据文件，同时把这次 Lookup 记成 Miss（`full_hit_count` 回退、`miss_count` +1），避免统计把一次失败命中算成命中。
- **命中路径 I/O 翻倍**：打开段时做整段校验，命中读取代价约为"纯读"的 2 倍（顺序读，页缓存命中后开销很小）。若后续需要优化，可改为在元数据中登记整段校验和，按需跳过逐块校验。
