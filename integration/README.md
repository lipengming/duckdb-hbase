# DuckDB-HBase 集成测试包

> 客户交易宽表 mock 数据 + HBase 夹具装载 + pytest 端到端用例。
> 本包是**独立的 Python 包**（`duckdb-hbase-integration`），不参与 Extension / Gateway 构建。
>
> - 环境安装（JDK 11 / HBase 2.6.6 / DuckDB CLI 2.0.0）：见 [docs/environment.md](../docs/environment.md)
> - 测试分层与验收标准：见 [docs/testing.md](../docs/testing.md) §5

## 1. 目标

1. **一条命令造数据**：生成 100 万行中文「客户交易宽表」mock 数据，写入 HBase；
   单客户数据支持 **10 万 / 50 万 / 100 万** 三个档位。
2. **数据可复现**：固定 seed 后逐字节一致（sha256 校验），manifest 固化生成参数与抽样行。
3. **用例分两层**：
   - **数据面**：只依赖 HBase，先证明夹具本身正确（表结构、RowKey、行数、时间跨度、取值语义）；
   - **端到端**：DuckDB + `hbase.duckdb_extension` + Gateway + HBase，覆盖投影/过滤/范围/分页/聚合/缓存。
4. **不误报**：环境缺失（未构建 Extension、Gateway 未启动、scan 数据面未实现）时用例 **skip**
   并打印修复命令；M1 完成后自动开始执行，无需改测试代码。

## 2. 前置条件

| 依赖 | 要求 | 说明 |
| --- | --- | --- |
| HBase | 2.6.x 已启动，`hbase` 在 PATH | 本包通过 HBase 自带 Java 客户端读写 |
| JDK | 11（`java` / `javac`） | 首次运行自动编译内置装载器（约 1 秒） |
| Python | 3.9+，pytest | 测试与 CLI |
| duckdb（Python） | 与 extension 构建版本一致（本仓库基线 2.0.0） | 仅端到端用例需要；缺失时自动 skip |
| Extension | `build/release/extension/hbase/hbase.duckdb_extension` | 可用 `DH_IT_EXTENSION` 指定其他路径 |
| Gateway | `grpc://127.0.0.1:18080`（可用 `DH_IT_GATEWAY` 修改） | 仅端到端用例需要 |

无需 HappyBase / Thrift / REST：装载与校验都走 HBase 客户端，Python 侧无第三方 HBase 依赖。

## 3. 目录结构

```text
integration/
├── pyproject.toml                    # 独立包定义（含 pytest 配置与 markers）
├── src/duckdb_hbase_it/
│   ├── config.py                     # 环境变量与路径（DH_IT_*）
│   ├── schema.py                     # 列族/列/RowKey 规则/salt 与范围计算
│   ├── generator.py                  # 确定性中文 mock 数据生成
│   ├── manifest.py                   # 数据集 manifest（生成/抽样/装载统计）
│   ├── loader.py                     # HBase 读写封装（编译并调用 Java 装载器）
│   ├── duckdb_session.py             # DuckDB + extension 初始化、能力探测
│   ├── cli.py                        # generate / load / verify / status / all
│   └── java/HBaseBulkLoader.java     # BufferedMutator 批量装载、count、scan
├── tests/
│   ├── test_generator.py             # 生成器/Schema 单元测试（无需 HBase）
│   ├── test_sql_builder.py           # hbase_scan SQL 拼装与转义单测
│   ├── test_fixture_hbase.py         # IT-DATA-*：数据面用例
│   ├── test_extension_scan.py        # IT-EXT-*：扫描语义与数据一致性
│   ├── test_extension_cache.py       # IT-CACHE-*：Full/Partial Hit、清理
│   ├── test_scale_tiers.py           # IT-SCALE-*：10 万/50 万/100 万三档
│   └── test_extension_errors.py      # IT-ERR-*：错误路径与空范围
└── .cache/                           # 生成的数据、manifest、编译产物、hosts（gitignored）
```

## 4. 数据模型：客户交易宽表

表：`duckdb_test:customer_transaction`，3 个列族，15 列，**16 个预分区**。

| 列族 | 列（qualifier） | 含义 | 示例 |
| --- | --- | --- | --- |
| `party` | `payer_id` / `payer_name` | 付款方 ID / 名称 | `U000001` / `钱娟` |
| `party` | `payee_id` / `payee_name` | 收款方 ID / 名称 | `M00019` / `海底捞` |
| `party` | `direction` | 方向 | `支出` / `收入` |
| `txn` | `txn_time` | 交易时间（+08:00） | `2025-09-01 21:30:11` |
| `txn` | `txn_ts` | 交易时间戳（毫秒） | `1756733411000` |
| `txn` | `txn_date` | 交易日期（按日聚合用） | `2025-09-01` |
| `txn` | `amount` | 金额（十进制字符串） | `20463.95` |
| `txn` | `currency` | 币种 | `CNY` / `USD` / `EUR` / `HKD` / `JPY` |
| `txn` | `status` | 状态 | `成功` / `退款` / `处理中` |
| `ext` | `product_id` / `product_name` | 交易产品 | `P0021` / `电动牙刷` |
| `ext` | `logistics_no` | 物流单号（虚拟商品无） | `ZTO002212669809` |
| `ext` | `remark` | 备注 | `客户备注：请尽快发货` |

### 4.1 RowKey 设计

```text
<salt:2hex><user_id>#<yyyymmdd>#<seq:6 位零填充>
例：0dU000001#20250901#000042
```

- `salt = crc32(user_id) % 16`，两位小写十六进制。**前缀由 user_id 打散**：不同客户落到
  16 个预分区；同一客户的所有行仍连续，因此仍可用**前缀范围扫描**取单客户全量数据。
- `user_id + date(到日)` 是需求要求的主干；同一天内增加 `seq`（1 开始）作为**唯一后缀**。
  仅用 `user_id + date` 时，一个客户一个月最多 31 个 RowKey，无法容纳 10 万/50 万/100 万行。
  这是对需求 RowKey 的必要扩展，测试用例按 `salt+user_id+"#"` 前缀取数，不依赖 seq 语义。
- 单客户范围：`[0dU000001#, 0dU000001$)`（末位 `#` 加一得到 `$`，严格上界，不误伤相邻客户）。
- 单客户单日范围：`[0dU000001#20250915, 0dU000001#20250915$)`，用于按天分页。

对应 SQL（Gateway 与 Extension 就绪后）：

```sql
SELECT * FROM hbase_scan('it', 'duckdb_test', 'customer_transaction',
                         start_row := '0dU000001#',
                         stop_row  := '0dU000001$');
```

### 4.2 空值语义

- Cell **不存在**（生成时用 `\N` 占位、装载时跳过）→ 读回 **NULL**；
- Cell **存在但值为空串**（如部分备注）→ 读回 **`''`**；
- 用例同时覆盖两者（`test_null_versus_empty_string` / `test_null_and_empty_value_semantics_present`）。

### 4.3 时间跨度

默认 `2025-09-01 ~ 2025-09-30`（30 天，一个自然月；`+08:00` 固定时区，避免机器时区影响）。
可用 `--base-date` / `--days` 修改，但校验要求落在**同一个自然月**内。

## 5. 数据规模与三档切换

默认（等于需求口径）：

| 项 | 值 |
| --- | --- |
| 总行数 | 1,000,000 |
| 单客户（`U000001`）档位 | 100,000（10 万） |
| 普通客户 | 9,000 个，各 100 行（共 900,000） |
| 时间跨度 | 30 天，每天约 3,333 行（热客户） |
| Cell 数 | 约 1,440 万 |

三档通过重新生成 + 装载切换（同一时刻只装载一档，manifest 记录当前档位）：

```bash
# 10 万档（默认）
python -m duckdb_hbase_it all --rows 1000000 --hot-rows 100000 --force

# 50 万档
python -m duckdb_hbase_it all --rows 1000000 --hot-rows 500000 --force

# 100 万档（全部数据属于同一客户，单 Region 热点场景）
python -m duckdb_hbase_it all --rows 1000000 --hot-rows 1000000 --force
```

`test_scale_tiers.py` 会为三个档位各生成一个用例：当前档位执行，其余 skip 并打印切换命令。
小规模 smoke（不污染 1M 夹具）：`--rows 20000 --hot-rows 5000 --ordinary-customers 50`。

## 6. 快速开始

```bash
cd integration

# 1) 安装（建议独立 venv；pip 证书问题见 6.2）
python3 -m venv .venv && . .venv/bin/activate
pip install -e '.[test]'

# 2) 生成 + 装载 + 校验（首次约 1~3 分钟，取决于 HBase 与单 Region 写入速度）
python -m duckdb_hbase_it all

# 3) 确认 DuckDB + extension 初始化（扩展未构建时给出明确失败原因）
python -m duckdb_hbase_it bootstrap --connect

# 4) 数据面用例（不依赖 DuckDB / Extension）
pytest -m dataplane

# 5) 全量用例（Extension / Gateway 未就绪时相关用例自动 skip）
pytest
pytest --runslow              # 追加全表/30 天全量分页等慢用例
```

### 6.1 CLI

| 命令 | 作用 |
| --- | --- |
| `python -m duckdb_hbase_it generate` | 生成 TSV.gz + manifest（`--force` 覆盖） |
| `python -m duckdb_hbase_it load` | 建表（16 预分区）+ 装载（`--append` 追加） |
| `python -m duckdb_hbase_it verify` | 校验 Region 数、热客户行数、manifest 抽样单元格（`--full-count` 全表计数） |
| `python -m duckdb_hbase_it status` | 打印配置 / manifest / HBase 探活 |
| `python -m duckdb_hbase_it all` | 以上三步串联 |
| `python -m duckdb_hbase_it bootstrap [--connect]` | 初始化 DuckDB + extension；`--connect` 时注册 Gateway 并探测 `hbase_scan` 能力 |

### 6.2 环境变量

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `DH_IT_NAMESPACE` / `DH_IT_TABLE` | `duckdb_test` / `customer_transaction` | HBase 命名空间与表 |
| `DH_IT_TOTAL_ROWS` / `DH_IT_HOT_ROWS` | `1000000` / `100000` | 总行数 / 单客户档位 |
| `DH_IT_DAYS` / `DH_IT_BASE_DATE` / `DH_IT_SEED` | `30` / `2025-09-01` / `20250901` | 时间跨度 / 起始日 / 随机种子 |
| `DH_IT_ORDINARY_CUSTOMERS` | `9000` | 普通客户数（瓜分剩余行数） |
| `DH_IT_DATA_DIR` | `integration/.cache/datasets` | 数据文件与 manifest 目录 |
| `DH_IT_EXTENSION` | 自动探测 `build/{release,debug}/...` | `hbase.duckdb_extension` 路径 |
| `DH_IT_GATEWAY` / `DH_IT_CONNECTION` | `grpc://127.0.0.1:18080` / `it` | Gateway 端点与连接名 |
| `DH_IT_DUCKDB` | 空（内存库） | Python 连接使用的 DuckDB 数据库文件 |
| `DH_IT_HOSTS_FILE` | 自动（仅本机 quorum） | 传给 JVM 的 `-Djdk.net.hosts.file`，见 §8.2 |

> **pip 证书问题**：部分 macOS Python 环境访问 pypi 会报
> `CERTIFICATE_VERIFY_FAILED`，可加
> `--trusted-host pypi.org --trusted-host files.pythonhosted.org`。

### 6.3 pytest 选项

| 选项 | 作用 |
| --- | --- |
| `-m dataplane` / `-m extension` / `-m cache` / `-m scale` | 按层选择用例 |
| `--runslow` | 执行全表/30 天全量分页等慢用例（默认 skip） |
| `--manifest PATH` | 指定 manifest（默认 `DH_IT_DATA_DIR/manifest.json`） |

## 7. 用例清单

数据面用例（只依赖 HBase）：

| 编号 | 用例 | 断言 |
| --- | --- | --- |
| IT-DATA-01 | `test_table_exists_with_expected_regions` | 表存在；Region 数 = 16；列族 = 3 |
| IT-DATA-02 | `test_total_row_count`（slow） | 全表行数 = manifest 记录值（默认 1,000,000） |
| IT-DATA-03 | `test_single_customer_row_count` | 单客户前缀范围行数 = 档位（10 万/50 万/100 万） |
| IT-DATA-04 | `test_each_day_has_expected_rows`（slow） | 30 天每天行数与规划一致 |
| IT-DATA-05 | `test_rowkey_layout_and_order` | RowKey 正则、salt 正确、按字节序单调 |
| IT-DATA-06 | `test_prefix_range_isolation` | 单客户前缀不串号、无重复行、范围外不误伤 |
| IT-DATA-07/08 | `test_party_amount_currency_semantics` | 收付款方/方向/金额/币种/产品/物流/时间戳一致性 |
| IT-DATA-09 | `test_manifest_samples_roundtrip` | manifest 抽样单元格与 HBase 完全一致 |
| IT-DATA-10 | `test_null_versus_empty_string` | 缺失 Cell = NULL；空串 Cell = `''` |
| IT-DATA-11 | `test_sample_row_values_are_chinese` | 中文内容未被编码破坏 |

端到端用例（DuckDB + Extension + Gateway；M1 数据面未实现时 skip）：

| 编号 | 用例 | 断言 |
| --- | --- | --- |
| IT-EXT-01 | `test_extension_loaded` | 扩展加载成功，`hbase_version()` 非空 |
| IT-EXT-02 | `test_connect_and_disconnect` | 连接可重复注册；重复 disconnect 返回 false |
| IT-EXT-03 | `test_projection_and_schema` | 只返回请求列，列名与 HBase 一致 |
| IT-EXT-05 | `test_day_filter_and_count` | 首日/末日 count 与规划一致 |
| IT-EXT-06 | `test_currency_group_by` | 币种聚合结果与 mock 数据一致 |
| IT-EXT-07 | `test_limit_is_applied` | LIMIT 生效 |
| IT-EXT-07b | `test_pagination_by_day_covers_hot_customer`（slow） | 30 天分页行数求和 = 档位，无重复/遗漏 |
| IT-EXT-09 | `test_first_day_matches_generator` | 首日全量行与确定性 mock 数据逐行一致（多重集合） |
| IT-EXT-10 | `test_null_versus_empty_string` | Extension 侧 NULL / 空串语义正确 |
| IT-CACHE-08 | `test_full_hit_does_not_rescan` | 重复查询结果一致；远端 Scan 增量为 0 |
| IT-CACHE-09 | `test_partial_hit_backfills_columns` | 补列后行数不变、新列正确、Get 计数增加 |
| IT-CACHE-10 | `test_cache_disabled_matches_enabled` / `test_cache_clear` | 开关与清理不改变结果 |
| IT-SCALE-11 | `test_single_customer_tier` | 三档行数（10 万/50 万/100 万）与 HBase、Extension 一致 |
| IT-ERR-12 | `test_unregistered_connection` / `invalid_column_spec` / `invalid_max_versions` / `gateway_unreachable` / `empty_range_returns_zero_rows` | 错误可诊断；空范围返回 0 行 |

`hbase_cache_stats()` 的输出列仍是方案中的 TBD：缓存用例按列名关键字识别计数列，
识别不到时 skip 并注明 TBD，结果正确性断言始终执行。

## 8. 实现说明

### 8.1 为什么用 Java 装载

1M 行 × 15 列 = **1,440 万 Cell**，`hbase shell` 逐行 `put` 需要数小时，不可用于可重复的
集成夹具。`java/HBaseBulkLoader.java` 使用 HBase 2.x `BufferedMutator`（8MB 写缓冲）批量写入，
并提供 `ping` / `count`（FirstKeyOnlyFilter）/ `scan` 三个只读子命令供测试断言。

- 编译产物缓存于 `.cache/loader-classes/`，源码变更自动重编译；
- classpath 来自 `hbase classpath`，不引入额外依赖；
- 进度写 stderr，结构化结果（JSON/TSV）写 stdout；
- 1M 行默认档位实测装载时间取决于 HBase 堆与 Region 分布；单客户 100 万行会集中到
  一个 Region（热点键场景），这是需求本身要覆盖的形状。

### 8.2 本机 DNS 反查 workaround

HBase 客户端建连时会调用 `InetAddress.getLocalHost()`；macOS 上该反查可能耗时 5~6 秒，
导致每次 JVM 调用都有固定开销。当 ZooKeeper quorum 指向本机时，本包自动生成
`.cache/hosts`（`127.0.0.1 localhost` + 本机 hostname）并通过 `-Djdk.net.hosts.file` 传给 JVM；
**quorum 非本机（远端集群）时不启用**，也可用 `DH_IT_HOSTS_FILE` 显式指定或禁用。

### 8.3 可复现性

- 行级取值全部来自 FNV-1a 混合（`seed + user_index + day + seq`），不依赖 Python
  `random`/`hash()` 与机器；
- 时间固定 `+08:00`；
- 生成结果记录未压缩 TSV 的 sha256，同一 seed 重复生成逐字节一致（单元测试覆盖）。

### 8.4 校验策略

`verify` 与数据面用例不重扫全表：manifest 中固化了 10+ 条抽样行（含月初/月末、热客户/普通客户、
NULL/空串），装载后按 RowKey `Get` 逐单元格比对；行数用 `FirstKeyOnlyFilter` 计数。
全表计数与 30 天逐日计数标记为 slow，按需开启。

## 9. 数据重置与清理

```bash
# 重新生成并重建表（load 默认 drop 后重建）
python -m duckdb_hbase_it all --force

# 只重建表、复用已生成数据
python -m duckdb_hbase_it load

# 删除表（方式一：HBase shell）
printf "disable 'duckdb_test:customer_transaction'\ndrop 'duckdb_test:customer_transaction'\n" | hbase shell -n

# 清理生成物与编译缓存
rm -rf integration/.cache
```

## 10. 已知限制 / TBD

| 项 | 现状 | 备注 |
| --- | --- | --- |
| `hbase_scan` 数据面 | M1 未实现（Arrow IPC） | 端到端用例自动 skip；实现后自动执行 |
| `hbase_cache_stats()` 列结构 | 方案 TBD | 用例按关键字识别，识别不到则 skip |
| `hbase_get` / `hbase_cache_clear` | 未注册 | 对应用例 skip |
| 并发 / 取消 / 失败注入 | 未覆盖 | 规划在 M3+；需要 Gateway 侧计数与故障注入能力 |
| 单客户 100 万行 | 单 Region 热点 | 符合「单客户扫描」场景；写入/扫描受单 Region 吞吐限制 |
| Java 调用固定开销 | 约 1.5 秒/次（含 JVM 启动） | 用例已控制调用次数；慢用例单独标记 |
| 远端集群 hosts workaround | 不启用 | 仅 ZooKeeper quorum 为本机时生成 |

## 11. 与仓库文档的关系

- 测试分层与验收标准：[docs/testing.md](../docs/testing.md) §5（本包对应用例清单在 §5.4）
- 本机环境与 HBase 夹具：[docs/environment.md](../docs/environment.md) §7
- 路线图：M1 完成 `hbase_scan` 数据面后，本包的 IT-EXT / IT-CACHE / IT-SCALE 用例转为可执行
