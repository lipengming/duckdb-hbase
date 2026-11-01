# 测试策略与质量保障

> 提炼自 [技术方案 V1.0](../tech_v0.1.md) §9-§11。若与方案冲突，以方案为准。

本文定义 DuckDB-HBase 的测试分层、用例清单、执行策略与暂缓项。模块划分与接口边界见 [architecture.md](architecture.md)、[extension-design.md](extension-design.md)、[cache-design.md](cache-design.md)；用例命名、断言风格与可测试性要求遵循 [coding-standards.md](coding-standards.md)。

## 1. 目标与原则

1. 双侧测试：C++ Extension 使用 DuckDB SQL Tests，Java 侧使用 JUnit，两侧用例编号可追溯。
2. 缓存正确性优先于缓存收益：任何"命中但数据错误"的缺陷视为最高级别缺陷。
3. 断言必须可观测：优先断言 `hbase_cache_stats()` 输出的命中状态、远端 Scan/Get 次数与行数，其次才是结果集比对。
4. 不预设加速倍数：性能测试只记录基线，不在用例中写死期望倍数。

## 2. 测试分层

| 层级 | 范围 | 依赖 | 运行时机 | 技术栈 |
| --- | --- | --- | --- | --- |
| 单元测试 | RowKey、ScanPlanner、Coverage Index、Merger、Storage、Provider 能力协商 | 无外部服务；Fake Provider 与内存 Storage 替代 Gateway / 文件系统 | 每次提交与 PR，全量 | C++ 单元测试框架（TBD）、JUnit（Java） |
| SQL 测试 | `hbase_scan`、`hbase_get`、`hbase_cache_*` 的 SQL 语义 | 本地 Extension + 可回放的录制数据 Provider | 每次 PR | DuckDB SQL Tests |
| 集成测试 | Extension + 真实 Gateway + 真实 HBase 2.x | HBase 2.x 集群、Gateway 进程、Parquet 缓存目录 | PR 跑 smoke 子集，nightly 跑全量 | 脚本驱动 SQL Tests + JUnit |
| 性能测试 | §9.3 四组对比场景与七项指标 | 固定规模数据集、固定硬件、禁用其他负载 | nightly 与发布前 | 基准脚本 + 结果归档 |
| 兼容性测试 | DuckDB 锁定版本、HBase 2.x 小版本、各 Adapter 能力矩阵 | 多版本构建矩阵 | 发布前、升级依赖时 | 构建矩阵复用集成用例 |

## 3. 单元测试清单

### 3.1 RowKey 解析与范围

- 二进制 RowKey 的字节序比较：带前缀、含 `0x00`、含高位字节、长度不等。
- `RowKeyRange` 的四种开闭组合，`include_start` / `include_stop` 为 false 时的边界剔除。
- 空 RowKey、单字节 RowKey、超长 RowKey 的处理。
- VARCHAR 输入按 UTF-8 转字节，BLOB 输入保持原始字节（同一字符串的两种输入路径结果一致）。
- 非法输入（NULL、空字符串、start > stop）的错误类型与错误信息。

### 3.2 Scan Planner

- `Output Columns` 与 `Filter Columns` 分离；`Required Columns = 并集`，缺一即失败。
- 投影下推：`SELECT name ... WHERE age > 30` 必须请求 `{name, age}`。
- `columns := [...]` 显式列表与 `SELECT *` 的展开结果。
- Filter 解析：可下推谓词与仅本地求值谓词的划分，不支持下推时不得丢弃过滤条件。
- `RowKeyRanges` 计算：单范围、多范围、无范围（全表）、与投影/过滤的组合。
- `LIMIT` 不得改变缓存写入范围；带 LIMIT 的查询不得产出"标记为完整范围"的 Segment。
- `ScanConsistency` 写入 ScanPlan 后与 Cache Lookup 的键一致。

### 3.3 Coverage Index / Cache Lookup

- 全命中：范围、列、时间戳语义、TTL、完整性五项同时满足才返回 `FullHit`。
- 部分命中：列缺口、范围缺口、两者并存时 `uncached_ranges` 的计算。
- 完全未命中。
- 多 Segment 拼接后的覆盖判定：相邻、重叠、包含、完全分离、仅缺口一个字节。
- TTL 过期后立刻降级，过期 Segment 不参与覆盖。
- Segment 状态非 `COMMITTED` 时不得被视为可用。
- 覆盖比较必须使用与 HBase 一致的字节序，禁止按字符串字典序比较。

### 3.4 Cache Merger

- 按 RowKey 对齐：左右两侧 RowKey 集合互有差异时的外连接语义。
- 列不存在 → NULL；列存在但值为空 → 空值而非 NULL，两者必须可区分。
- 重复 RowKey（同一 Segment 内）的确定性处理。
- 合并结果的列顺序、类型与 Schema 一致；空 Segment 参与合并不产生脏数据。
- Copy-on-Write：合并只产生新 Segment，原 Segment 不被就地修改。

### 3.5 Cache Storage 与 Metadata

- Parquet 写入后可读回，行数、Schema、RowKey 顺序一致。
- 文件损坏（截断、字节翻转、Schema 不匹配）时校验失败并 Abort，不影响已有 Segment。
- `staging` 临时文件在 Abort 后清理干净。
- 原子 Metadata Commit：提交前崩溃则新 Segment 不可见，提交后崩溃则旧 Segment 仍可读。
- SQLite 元数据表的 `status`、`generation`、`expires_at` 字段的读写与迁移。
- 并发 BeginWrite 同一范围时，Generation CAS 使其中一个失败而非覆盖。

### 3.6 类型映射与 DataChunk 转换

- HBase 字节到 DuckDB 逻辑类型（`logical_type`）的映射与反向写出。
- NULL 与空值在 Arrow → DataChunk 转换中的区分。
- `max_versions > 1` 时多版本展开的列结构。
- 越界值、非法 UTF-8 字节、超长 BLOB 的降级或报错路径。

### 3.7 Provider 能力协商与错误映射

- 能力位组合：不支持 Batch Get 时 Partial Hit 退化为范围 Scan；不支持时间戳时拒绝时间戳语义请求。
- 缺失必需能力时的显式失败，而非静默降级。
- 远端错误（超时、权限、表不存在、Scanner 失效、Gateway 不可达）到本地错误类型的映射。
- `Cancel` 幂等；重复取消不产生异常。
- 连接初始化阶段能力获取失败的处理。

### 3.8 Gateway Adapter

- `scan()` 与 `batchGet()` 的请求/响应转换，含空结果与多 Family 结果。
- Native Java Adapter 与 REST Adapter 对同一逻辑请求的等价输出。
- Phoenix Adapter 的独立语义测试：SQL 结果集语义与原生 Cell Scan 语义的差异不得混用。

## 4. SQL 测试（`hbase_scan`）

| 类别 | 用例 | 断言要点 |
| --- | --- | --- |
| 投影 | `SELECT *`、列子集、单列、重复列、`columns := ['cf1:name']` | 返回列与顺序符合 Schema；未请求列不下发给 Gateway |
| 过滤 | `WHERE age > 30`、多条件、不可下推函数、`WHERE` 引用未 SELECT 的列 | Required Columns 包含过滤列；过滤后行数正确 |
| 范围 | 默认全表、`start_row`/`stop_row`、左闭右开边界、start = stop、start > stop | 边界行包含/排除正确；空范围返回 0 行且不报错 |
| 二进制 RowKey | BLOB 字面量作为 `start_row`/`stop_row`、含 `0x00` | 不发生 UTF-8 重编码 |
| NULL | 列不存在、列存在但空值、整行缺失 | 分别映射为 NULL / 空值 / 无行 |
| 类型转换 | `VARCHAR` 与 `BLOB` 列、显式 `CAST`、非法字面量 | 转换结果与 DuckDB 类型规则一致；非法输入报错可读 |
| 空结果 | 命中 0 行的表 / 范围 | 仍返回正确 Schema 的空结果集 |
| 大结果集 | 远大于单批次的表、`LIMIT`、无 `LIMIT` 全量 | 流式分块输出，内存不随结果集线性增长 |
| 版本 | 默认 `max_versions := 1`、`max_versions := 5`、`timestamp_min`/`timestamp_max` | 不同版本语义不复用同一缓存 |
| 缓存开关 | `cache := false`、`SET hbase_cache_enabled = false` | 强制走远端且不写缓存 |
| Get | `hbase_get(rowkeys := [...])` 含不存在 RowKey | 缺失 RowKey 不产生行；不写入负结果缓存 |
| 管理函数 | `hbase_cache_stats()`、`hbase_cache_clear()`、`hbase_tables()`、`hbase_columns()` | 返回结构与文档一致；清理后命中状态归零 |

## 5. 集成测试

### 5.1 环境矩阵

- 真实 HBase 2.x（版本待定，TBD），至少覆盖 Native Java Adapter 与 HBase REST Adapter。
- Thrift Adapter 是否纳入首批支持待定（TBD）；纳入后才进入矩阵。
- Phoenix Adapter 单独成组，只验证其自身语义。

### 5.2 测试数据准备

- 固定表集：单 Family 窄表、多 Family 宽表、含二进制 RowKey 表、含历史版本表、空表。
- 数据规模分两档：功能数据集（可人工核对，行数固定）与规模数据集（用于性能与流式验证）。
- 每轮集成测试前重建表并写入确定性数据，禁止依赖上一轮的残留状态。

#### 客户交易宽表（规模数据集）

`integration/` 包提供可重复生成的中文交易宽表夹具 `duckdb_test:customer_transaction`：
总行数 100 万，单客户（热客户）支持 **10 万 / 50 万 / 100 万** 三档，时间跨度 30 天
（默认 2025-09-01 ~ 2025-09-30，`+08:00`），3 个列族、15 列（收付款方 id+名称、时间、金额、
币种、产品、物流单号、备注等），16 个按 `crc32(user_id) % 16` 预分区的 Region。

- RowKey：`<salt:2hex><user_id>#<yyyymmdd>#<seq:6d>`。`user_id + date` 为主干，
  `seq` 是同日唯一后缀（仅 `user_id + date` 一个月最多 31 个 RowKey，无法容纳单客户 10 万行）；
  单客户前缀范围 `[<salt><user_id>#, <salt><user_id>$)` 与单日范围均可直接用于 Scan。
- 生成/装载/校验由 `python -m duckdb_hbase_it` 完成，数据与 manifest 写入 `integration/.cache/`；
  同一 seed 逐字节可复现，manifest 记录抽样行供测试逐单元格比对。
- 空值语义：Cell 缺失读回 NULL，Cell 存在但空串读回 `''`，两者均有专门用例覆盖。

### 5.3 端到端断言

| 场景 | 步骤 | 断言 |
| --- | --- | --- |
| Cache Miss | 首次查询 | 远端 Scan 次数 ≥ 1；生成 `COMMITTED` Segment；结果与直连 HBase 一致 |
| Full Hit | 同参数重复查询 | 远端 Scan 次数增量为 0；`hbase_cache_stats()` 报告全命中 |
| Partial Hit | 增加新列再次查询 | 触发 Batch Get 补齐缺失列；行数不增加；新旧列值均正确 |
| 部分范围命中 | 缩小 RowKey 范围重复查询 | 只对缺口范围发起 Scan，已覆盖范围不重复扫描 |
| 失效 | TTL 到期或 `hbase_cache_clear()` 后查询 | 重新访问远端；旧 Segment 不可读 |
| 能力退化 | 关闭 Gateway 的 Batch Get 能力 | Partial Hit 退化为范围 Scan，结果仍正确 |
| 取消 | 查询中途取消 | 远端 Scanner 与本地 Reader 均关闭，无残留文件 |

### 5.4 可执行集成测试包（`integration/`）

§4、§5.3 的用例已落成独立 Python 包，按「数据面」与「端到端」两层实现，用例编号可追溯：

| 层 | 文件 | 用例编号 | 依赖 | 运行方式 |
| --- | --- | --- | --- | --- |
| 数据面 | `integration/tests/test_fixture_hbase.py` | IT-DATA-01 ~ 11 | HBase 2.6.x + JDK 11 | `pytest -m dataplane` |
| 端到端 | `integration/tests/test_extension_scan.py` | IT-EXT-01 ~ 10 | + DuckDB + Extension + Gateway | `pytest -m extension` |
| 缓存 | `integration/tests/test_extension_cache.py` | IT-CACHE-08 ~ 10 | 同上 | `pytest -m cache` |
| 规模 | `integration/tests/test_scale_tiers.py` | IT-SCALE-11 | 同上 | `pytest -m scale` |
| 错误路径 | `integration/tests/test_extension_errors.py` | IT-ERR-12 | 同上 | `pytest -m extension` |
| 单测（生成器 / SQL 拼装） | `integration/tests/test_generator.py`、`test_sql_builder.py` | — | 无 | `pytest tests/test_generator.py tests/test_sql_builder.py` |

执行约定：

- 数据夹具（manifest + 已装载的表）缺失、Extension 未构建、Gateway 未启动、
  `hbase_scan` 数据面未实现（M1 进行中）时，用例 **skip 并打印修复命令**，不误报为产品缺陷；
  实现完成后自动开始执行，无需修改测试；
- 全表计数、30 天逐日分页等大范围用例标记 `slow`，默认跳过，`--runslow` 开启；
- `hbase_cache_stats()` 输出列仍是 TBD，缓存用例按列名关键字识别计数列，识别不到时 skip；
- 详细的数据模型、三档切换、CLI、环境变量与已知限制见 [`integration/README.md`](../integration/README.md)。

## 6. 正确性测试重点

- **RowKey 语义**：缓存覆盖比较、范围裁剪、跨 Segment 拼接必须与 HBase 字节序一致；任何按字符串排序的实现视为缺陷。
- **版本语义**：不同 `max_versions`、不同 `timestamp_min`/`timestamp_max` 的 Scan 不得复用同一缓存；无法证明时间戳兼容时必须回退为 Cache Miss。
- **部分补列一致性**：补列以 RowKey 为主键；必须区分"列存在且值为空""列不存在""RowKey 不存在""RowKey 在查询期间变化"四种情况。Batch Get 未返回某 RowKey 时不得写入负结果缓存。
- **并发查询**：构造 A/B 缺同一列、C 缺另一列的并发场景，断言 A/B 合并为一次补列请求、C 独立请求；并发提交通过 Generation CAS 保证只有一个成功，另一侧重试或放弃，不出现丢失更新。
- **失败恢复**：按 `INIT → WRITING → VALIDATING → COMMITTED` 四阶段注入失败。INIT 终止、WRITING 删除临时文件、VALIDATING 校验失败则 Abort、COMMITTED 不回滚已发布 Segment；已提交数据在后续查询中仍可读。
- **重试不得拼接数据**：已返回部分结果的 Scan 不得从头重试并把新数据追加到旧结果上，断言结果行无重复、无缺失。
- **取消与幂等**：取消只释放资源，不提交半成品；重复取消不报错。

## 7. 性能测试

对比四组数据：直接 HBase Scan；首次 Scan + Cache Write；第二次 Full Cache Hit；第二次 Partial Cache Hit。

| 指标 | 采集方式 | 目标 |
| --- | --- | --- |
| Query Latency（含 P50 / P95） | 基准脚本逐查询打点 | 记录值，不预设门限（TBD） |
| HBase Scan Count | Gateway 侧计数 + Extension 统计 | 记录值；Full Hit 场景期望增量为 0 |
| HBase Get Count | Gateway 侧计数 | 记录值；Partial Hit 场景按缺列批次统计 |
| Cache Hit Rate | `hbase_cache_stats()` | 记录值（TBD 基线） |
| Memory Usage | 进程 RSS 与 DuckDB 内存统计 | 随结果集规模不线性增长 |
| Cache Write Throughput | 写入行数 / 写入耗时 | 记录值（TBD 基线） |
| Gateway Throughput | Gateway 侧请求与 Arrow 批次统计 | 记录值（TBD 基线） |

压测方法：固定数据集与硬件，单变量对比四组场景；每组预热后重复多轮取分位值，同时记录数据规模、HBase 版本、Gateway 配置与 Extension 版本。基线结果归档为版本化文件并在 PR 中更新，禁止只保留最后一次结论。

## 8. 测试数据与夹具管理

- 夹具定义（表结构、RowKey 生成规则、预期结果）随代码入库，二进制数据由脚本生成而非手工提交。
- 单元测试使用内存夹具；SQL 测试使用录制回放数据；集成测试使用真实表。
- 夹具变更需同时更新预期结果，并在 PR 描述中说明影响范围。

## 9. CI 执行策略

| 阶段 | 执行内容 |
| --- | --- |
| PR（必需） | 全量单元测试、SQL 测试、编译与静态检查、集成 smoke 子集 |
| PR（可选，按改动路径触发） | 涉及 cache 的改动跑缓存集成用例；涉及 Gateway 的改动跑对应 Adapter 用例 |
| Nightly | 全量集成测试、四组性能对比、并发与失败注入用例 |
| 发布前 | 兼容性矩阵、性能基线归档、Kerberos/TLS 环境验证 |

集成测试不可用时，PR 必须至少通过单元测试与 SQL 测试，且在 PR 中标注未验证项。

## 10. 暂不覆盖与待定项

- 写入路径（INSERT/UPDATE/DELETE）不测：第一阶段只读。
- DuckDB Catalog 集成不测：第一版不实现 Catalog。
- CDC 驱动的缓存失效不测：方案列为远期能力。
- 断点续传不测：需要稳定的扫描语义与最后处理 RowKey，尚未设计。
- 多节点缓存共享不测：缓存默认属于单个 DuckDB 进程。
- 待定（TBD）：HBase 2.x 具体测试版本、Thrift 是否进入首批、性能基线与门限数值、集成环境的容器化方案、Kerberos 测试环境获取方式。
