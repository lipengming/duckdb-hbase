# DuckDB-HBase 用户使用指南

> 提炼自 [技术方案 V1.0](../tech_v0.1.md) §4-§5、§8。若与方案冲突，以方案为准。

## 1. 适用读者与前置条件

本指南面向使用 SQL 或 Python 查询 HBase 的数据分析师与数据工程师，只需安装 Extension、配置连接、执行 SQL、查看缓存效果，无需手工构造 Scan Request 或管理 Arrow 数据流。

| 前置条件 | 要求 |
| --- | --- |
| DuckDB 与 Extension | DuckDB 版本与 Extension 构建时锁定的版本一致（版本号 TBD），且已获得匹配的 `hbase.duckdb_extension` 构建产物 |
| HBase | 2.x，Gateway 侧使用 HBase Java Client 2.x |
| Java Gateway | 已独立部署并启动，查询主机可访问其 gRPC 端点。可用 `curl http://127.0.0.1:18080/health` 确认可达，响应体格式待定（TBD）；Extension 不负责安装或启动 Gateway |

## 2. 安装

### 2.1 安装 DuckDB 与 Python 包

DuckDB、Extension、Java Gateway 是三个独立的安装对象，本节只涉及前两者，Gateway 见[部署运维指南](deployment-guide.md)。

```bash
pip install duckdb          # Python 用户
curl -LO https://github.com/duckdb/duckdb/releases/latest/download/duckdb_cli-linux-amd64.zip
unzip duckdb_cli-linux-amd64.zip
./duckdb                    # 启动 DuckDB CLI
```

生产部署应固定 DuckDB 版本，不要始终使用 latest。Python 侧用 `duckdb.connect()` 打开数据库即可，见第 6 节。

### 2.2 安装 Extension

方式 A 为本地文件加载，开发阶段最简单。假设编译产物为 `build/release/extension/hbase/hbase.duckdb_extension`：

```sql
LOAD '/opt/duckdb/extensions/hbase.duckdb_extension';
```

动态库文件名以构建产物为准。方式 B 为 Extension Repository，发布后使用：

```sql
INSTALL hbase FROM community;
LOAD hbase;
```

方式 B 属于未来发布后的形式，不代表当前已经发布。

## 3. 连接与配置

### 3.1 注册连接

```sql
SELECT hbase_connect('prod', 'grpc://127.0.0.1:18080');

SELECT hbase_connect('prod', '{"endpoint": "grpc://127.0.0.1:18080", "timeout_ms": 30000, "cache_enabled": true, "cache_ttl": 300}');
```

第二个参数支持端点字符串或 JSON 配置，连接名（如 `prod`）供后续查询引用。生产环境不要把密码写入 SQL 明文，应通过环境变量、Kerberos 或受保护的配置文件获取凭据。

| 参数名 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `endpoint` | VARCHAR | 无（必填） | Gateway 地址，形如 `grpc://host:port` |
| `timeout_ms` | INTEGER | 待定（TBD，示例 30000） | 客户端请求超时，单位毫秒 |
| `cache_enabled` | BOOLEAN | 待定（TBD，示例 true） | 该连接是否启用缓存 |
| `cache_ttl` | INTEGER | 待定（TBD，示例 300） | 缓存有效期，单位秒 |

### 3.2 `hbase_scan` 参数

签名：`hbase_scan(connection_name, namespace, table_name, start_row := NULL, stop_row := NULL, columns := NULL, max_versions := 1, timestamp_min := NULL, timestamp_max := NULL, cache := true)`，参数含义如下。

| 参数名 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `connection_name` | VARCHAR | 无（必填） | 已注册的连接名 |
| `namespace` | VARCHAR | 无（必填） | HBase 命名空间，如 `default` |
| `table_name` | VARCHAR | 无（必填） | 表名 |
| `start_row` | VARCHAR 或 BLOB | NULL（不限下界） | 起始 RowKey，左闭；VARCHAR 按 UTF-8 转字节，BLOB 保持原始字节 |
| `stop_row` | VARCHAR 或 BLOB | NULL（不限上界） | 结束 RowKey，右开；类型处理同上 |
| `columns` | VARCHAR[] | 无（M1 必填） | 形如 `['cf1:name', 'cf1:age']`；省略会报错 |
| `max_versions` | INTEGER | 1 | 返回的最大版本数；M1 只支持 1，>1 报错 |
| `timestamp_min` | BIGINT | NULL | 时间戳下界 |
| `timestamp_max` | BIGINT | NULL | 时间戳上界 |
| `cache` | BOOLEAN | NULL | 本次查询是否使用缓存；M1 缓存未实现，显式 `true` 会报错 |

第一版区间为左闭右开；RowKey 是二进制字节数组，不能一律按 UTF-8 字符串处理，否则会破坏排序与范围比较语义。`cache` 控制单次查询，全局开关见 3.3。

### 3.3 缓存配置

```sql
SET hbase_cache_enabled = true;
SET hbase_cache_ttl = 300;
SET hbase_cache_max_size = '10GB';
SET hbase_cache_directory = './hbase_cache';
SET hbase_cache_mode = 'parquet';
```

各配置项默认值待定（TBD），方案示例值依次为：`hbase_cache_enabled` 布尔全局开关（true）、`hbase_cache_ttl` 秒级有效期（300）、`hbase_cache_max_size` 容量上限（`10GB`）、`hbase_cache_directory` 缓存目录（`./hbase_cache`）、`hbase_cache_mode` 存储模式（`parquet`）。`cache` 参数与全局开关同时为真才启用缓存。

## 4. 查询示例

### 4.1 基础 Scan、投影与过滤

```sql
SELECT * FROM hbase_scan('prod', 'default', 'customer_profile');

SELECT name, city
FROM hbase_scan('prod', 'default', 'customer_profile',
                columns := ['cf1:name', 'cf1:city']);

SELECT name, age
FROM hbase_scan('prod', 'default', 'customer_profile',
                columns := ['cf1:name', 'cf1:age'])
WHERE age > 30;
```

`WHERE` 使用的列即使不出现在 `SELECT` 中也会自动加入必需列集合（上例的 `age`），这是 Filter 正确执行的前提。

### 4.2 RowKey 范围与 Get

```sql
SELECT name, age
FROM hbase_scan('prod', 'default', 'customer_profile',
                start_row := '1000', stop_row := '2000',
                columns := ['cf1:name', 'cf1:age'])
WHERE age > 30;

SELECT * FROM hbase_get('prod', 'default', 'customer_profile',
                        rowkeys := ['1001', '1002', '1003']);
```

范围查询返回 RowKey 落在 `[1000, 2000)` 的数据；二进制 RowKey 请用 BLOB 形式传入。列信息可用 `hbase_columns()` 查看。

### 4.3 分页与批大小

SQL 参数中没有分页参数，`batch_size` 只存在于 Extension 与 Gateway 之间的扫描请求中（TBD）。分页应通过 RowKey 分段实现：以上一页的 `stop_row` 作为下一页起点，左闭右开不会重复返回边界行。

`LIMIT` 只限制返回行数，不能用于分页缓存：方案禁止只缓存 `LIMIT` 返回的数据并将其当作完整 RowKey 范围的缓存。批大小受 Gateway 能力协商中 `max_batch_size`、`max_parallel_scans` 约束，推荐值待定（TBD）。

## 5. 缓存使用

### 5.1 首次查询与二次命中

```sql
-- 首次查询：Cache Miss，访问 Gateway 与 HBase，并写入缓存
SELECT name, age, city
FROM hbase_scan('prod', 'default', 'customer_profile',
                start_row := '1000', stop_row := '2000')
WHERE age > 30;

-- 二次查询：相同范围、不同过滤条件，可复用缓存
SELECT name, city
FROM hbase_scan('prod', 'default', 'customer_profile',
                start_row := '1000', stop_row := '2000')
WHERE age > 40;
```

缓存以 Segment 为单位管理，不以 SQL 文本哈希为 Key。完整命中要求同时满足：RowKey 范围完整覆盖、必需列全部覆盖、时间戳语义兼容、缓存未过期、缓存数据完整。

### 5.2 部分命中

在已有范围上增加列属于 Partial Hit：逐段读取已缓存列，按 RowKey 向 Gateway 补齐缺失列，对齐后原子提交新 Segment。

```sql
SELECT name, age, city, phone
FROM hbase_scan('prod', 'default', 'customer_profile',
                start_row := '1000', stop_row := '2000');
```

不同 RowKey 范围可能具有不同的缺失列，命中结果按分段覆盖计划返回，而不是简单的 Full / Partial / Miss 三分类。

### 5.3 缓存管理

```sql
SELECT * FROM hbase_cache_stats();

SELECT hbase_cache_clear('prod', 'default', 'customer_profile');

SELECT hbase_cache_clear();
```

`hbase_cache_stats()` 的输出列方案未定义（TBD）。缓存失效包含 TTL 与手动失效，后续支持 CDC 失效。

## 6. Python API 示例

```python
import duckdb

con = duckdb.connect("analytics.duckdb")
con.execute("LOAD '/opt/duckdb/extensions/hbase.duckdb_extension'")
con.execute("SELECT hbase_connect('prod', 'grpc://127.0.0.1:18080')")
result = con.sql("""
    SELECT name, age, city FROM hbase_scan('prod', 'default', 'customer_profile',
                    start_row := '1000', stop_row := '2000') WHERE age > 30
""").fetchdf()
print(result)
```

## 7. 常见问题与排查

| 现象 | 可能原因 | 处理方式 |
| --- | --- | --- |
| 连接失败 | Gateway 未启动；`endpoint` 主机或端口错误；连接名不一致 | 先执行 `curl http://<host>:18080/health`；确认 `hbase_scan` 的第一个参数与注册名一致；重新注册连接 |
| Gateway 超时 | 扫描范围过大；`timeout_ms` 小于实际耗时；Gateway 到 HBase 的 `rpc_timeout_ms` 过小 | 缩小 RowKey 范围并显式指定 `columns`；调整超时；已返回部分结果的 Scan 不要从头重试后拼接旧数据 |
| 缓存未命中或意外回源 | TTL 过期；`WHERE` 列未被缓存；范围未完整覆盖；版本或时间戳语义不同；`cache := false` 或全局开关关闭；带 `LIMIT` 的结果不作为完整范围缓存 | 用 `hbase_cache_stats()` 观察；重复查询保持相同范围与列集；检查 `hbase_cache_enabled`、`hbase_cache_ttl`；改用 RowKey 分段 |
| 结果为空但 RowKey 存在 | 列不存在、Cell 不存在、时间戳范围过滤掉数据 | Cell 不存在按 HBase 语义映射为 NULL；核对 `timestamp_min` / `timestamp_max` 与 `max_versions` |
| RowKey 范围结果异常 | RowKey 被按 UTF-8 字符串处理，破坏二进制排序 | 二进制 RowKey 用 BLOB 输入；注意 `stop_row` 右开边界 |
| 列类型不匹配 | 列类型与 DuckDB 逻辑类型映射不一致 | 通过 `hbase_columns()` 核对 Family / Qualifier 与类型映射；映射规则待定（TBD） |

## 8. 性能建议

1. 列裁剪：始终显式指定 `columns`，避免 `SELECT *` 拉回整个列族；缓存也以实际读取的列为覆盖单位。
2. 范围收敛：用 `start_row` / `stop_row` 限定必要区间，区间越小，远端 Scan 与缓存写入成本越低。
3. 合理批大小：不要用单次查询拉全表，结果需流式消费，方案禁止全量结果无界加载内存。
4. 复用查询形态：缓存命中依赖范围与必需列同时覆盖，查询越稳定命中率越高；TTL 过短会频繁回源，过长会读到过期数据。
5. 关注磁盘占用与真实测量：`hbase_cache_max_size`、`hbase_cache_directory` 决定本地空间使用；Query Latency、HBase Scan Count、Cache Hit Rate 等指标须在真实 HBase 环境测量，方案不预设固定加速倍数。

## 9. 相关文档

- [总体架构](architecture.md)：分层与模块边界。
- [缓存设计](cache-design.md)与 [Gateway 设计](gateway-design.md)：Segment 与覆盖索引、失效策略、Provider 协议与 Adapter SPI。
- [部署运维指南](deployment-guide.md)：Gateway 部署、安全、容量与故障处置。
- [测试与质量保障](testing.md)：单元、集成与性能测试范围。
