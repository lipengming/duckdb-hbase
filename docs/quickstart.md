# 快速上手：用 DuckDB CLI 查询 HBase

> 适用版本：M1（Extension ↔ Gateway 数据面已打通，缓存尚未实现）。
> 环境准备见 [开发环境说明](environment.md)，完整 SQL API 见 [用户指南](user-guide.md)。

## 1. 前置条件

| 组件 | 要求 | 检查命令 |
| --- | --- | --- |
| HBase | 2.6.x 已启动 | `hbase shell -n <<< "status 'summary'"` |
| Gateway | 已启动，监听 18080 | `bash scripts/env-check.sh` |
| DuckDB CLI | v2.0.0（与扩展构建版本一致） | `duckdb --version` |
| Extension | `build/release/extension/hbase/hbase.duckdb_extension` | 见下 |

一次性拉起全部依赖（HBase + 夹具 + Gateway）：

```bash
cd /path/to/duckdb-hbase
bash scripts/env-up.sh
```

首次或改动代码后需要重新构建扩展（首次约 15-30 分钟）：

```bash
VCPKG_TOOLCHAIN_PATH="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" \
OVERRIDE_GIT_DESCRIBE=v2.0.0 GEN=ninja make release
```

> `OVERRIDE_GIT_DESCRIBE=v2.0.0` 用来让扩展的 DuckDB 版本串与官方 v2.0.0 CLI 对齐；
> 不加会出现 “built specifically for DuckDB version …” 的加载失败。

## 2. 三步查询

### 第 1 步：启动 DuckDB CLI（必须加 `-unsigned`）

本地构建的扩展没有签名，DuckDB 默认拒绝加载：

```bash
duckdb -unsigned
```

> macOS 上 `LOAD` 必须用**绝对路径**，否则会报 `relative path not allowed in hardened program`。

### 第 2 步：加载扩展并注册连接

```sql
LOAD '/Users/cubber.li/code/duckdb-hbase/build/release/extension/hbase/hbase.duckdb_extension';

SELECT hbase_connect('prod', 'grpc://127.0.0.1:18080');
-- true
```

`hbase_connect` 会做一次握手（Health + GetCapabilities）：Gateway 没起时直接报
`Gateway 不可用`，并且不会注册这个连接。

### 第 3 步：查询

```sql
SELECT "cf1:name", "cf2:age"
FROM hbase_scan(
    'prod', 'duckdb_test', 'customer_profile',
    start_row := 'r001',
    stop_row  := 'r011',
    columns   := ['cf1:name', 'cf2:age']
);
```

```
┌────────────┬─────────┐
│ cf1:name   │ cf2:age │
│  varchar   │ varchar │
├────────────┼─────────┤
│ user-001   │ 21      │
│ ...
```

## 3. `hbase_scan` 参数

```sql
hbase_scan(
    connection_name,     -- 必填：hbase_connect 注册的名字
    namespace,           -- 必填：HBase namespace
    table_name,          -- 必填：表名
    start_row := NULL,   -- 起始 RowKey（含），NULL = 不限
    stop_row  := NULL,   -- 结束 RowKey（不含），NULL = 不限
    columns   := NULL,   -- 列列表 ['family:qualifier', ...]
    max_versions := 1,   -- 返回的版本数
    timestamp_min := NULL,
    timestamp_max := NULL,
    cache := true        -- 预留：缓存引擎在 M3 实现，当前忽略
)
```

要点：

- **必须显式给 `columns`**。HBase 没有固定 schema，Gateway 只能拿到列族；不传 `columns` 会退化成整列族，列名不带 qualifier，通常不是你想要的结果。
- 列名一律用 `"family:qualifier"` 双引号引用（因为含冒号）。
- 所有列当前都是 `VARCHAR`（类型映射待定），数值需要自己 `CAST`：
  ```sql
  SELECT "cf2:age"::INTEGER AS age FROM hbase_scan(...);
  ```

## 4. 常用示例

```sql
-- 计行数 / 聚合
SELECT count(*) FROM hbase_scan('prod','duckdb_test','customer_profile',
                                columns := ['cf1:name']);

SELECT "cf1:city", count(*)
FROM hbase_scan('prod','duckdb_test','customer_profile', columns := ['cf2:city'])
GROUP BY 1 ORDER BY 2 DESC LIMIT 10;

-- 过滤（在 DuckDB 侧执行）
SELECT "cf1:name", "cf2:age"
FROM hbase_scan('prod','duckdb_test','customer_profile',
                columns := ['cf1:name','cf2:age'])
WHERE "cf2:age"::INTEGER > 40;

-- 只取前 N 行（注意：LIMIT 不会减少远端扫描，HBase 仍按范围扫描）
SELECT * FROM hbase_scan('prod','duckdb_test','customer_profile',
                         start_row := 'r001', stop_row := 'r011',
                         columns := ['cf1:name']) LIMIT 3;

-- 时间戳过滤（min/max_timestamp 已透传到 HBase）
SELECT * FROM hbase_scan('prod','duckdb_test','customer_profile',
                         columns := ['cf2:score'], timestamp_min := 1000, timestamp_max := 2000);

-- 断开连接
SELECT hbase_disconnect('prod');   -- true 表示删除成功
```

## 5. 一次执行（脚本 / CI）

```bash
duckdb -unsigned -c "
LOAD '$(pwd)/build/release/extension/hbase/hbase.duckdb_extension';
SELECT hbase_connect('prod','grpc://127.0.0.1:18080');
SELECT count(*) FROM hbase_scan('prod','duckdb_test','customer_profile', columns := ['cf1:name']);
"
```

SQL 文件方式：

```bash
cat > /tmp/q.sql <<'SQL'
LOAD '/abs/path/hbase.duckdb_extension';
SELECT hbase_connect('prod','grpc://127.0.0.1:18080');
SELECT "cf1:name" FROM hbase_scan('prod','duckdb_test','customer_profile',
                                  start_row := 'r001', stop_row := 'r005',
                                  columns := ['cf1:name']);
SQL
duckdb -unsigned -f /tmp/q.sql
```

## 6. 已知限制（M1）

| 限制 | 说明 / 规避 |
| --- | --- |
| 缓存未实现 | `cache := true` 参数被忽略，每次查询都访问 HBase（M3 实现） |
| 只有字符串类型 | 数值/时间需要 `CAST` |
| 单线程扫描 | 一个查询一个远端流；并行扫描在 M2 |
| 无 Catalog | 必须显式传 namespace / table，不支持 `SELECT * FROM hbase.table` |
| 只读 | 不支持写入、DDL |
| **按列扫描会跳过没有匹配列的行** | HBase 语义：只请求 `ext:remark` 时，remark 缺失的行不会返回。需要 NULL 语义时，**同时请求一个必然存在的列**（例如 `party:payer_id`），或先物化：`CREATE TABLE t AS SELECT ...` |
| 多版本未展开 | `max_versions` 会传给 HBase，但每个 cell 只保留最新版本，不会展开成多行 |
| **`IS NULL` / `FILTER` 结果不稳定** | 当前版本在扫描结果上直接 `col IS NULL` + `FILTER`/`DISTINCT` 会误判。**规避：先 `CREATE TABLE t AS SELECT ...` 再查 `t`**（已验证语义正确） |

## 7. 排错

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `dlopen(... relative path not allowed ...)` | macOS 安全策略 | `LOAD` 用绝对路径 |
| `signature is either missing or invalid` | 扩展未签名 | `duckdb -unsigned` |
| `built specifically for DuckDB version ...` | 扩展与 CLI 版本不一致 | 用同一 v2.0.0 构建，构建时加 `OVERRIDE_GIT_DESCRIBE=v2.0.0` |
| `hbase_connect: Gateway 不可用` | Gateway 未启动 | `bash scripts/env-up.sh` |
| 查询返回 0 行 | 范围写错（`stop_row` 是开区间）/ 表为空 / 只请求了缺失列 | 检查 RowKey 前缀；同时请求基础列 |
| `hbase_scan: 连接 'xxx' 未注册` | 没执行 `hbase_connect` | 先注册连接 |
| 中文乱码 | 终端或数据编码问题 | 确认数据是 UTF-8；扩展按字节透传 |

诊断命令：

```bash
bash scripts/env-check.sh        # 环境体检（工具链/HBase/夹具/Gateway/扩展）
bash scripts/env-down.sh         # 停止 Gateway 与 HBase
```
