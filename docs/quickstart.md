# 快速上手：用 DuckDB CLI 查询 HBase

> 适用版本：M1（Extension ↔ Gateway 数据面已打通，缓存尚未实现）。
> 环境准备见 [开发环境说明](environment.md)，完整 SQL API 见 [用户指南](user-guide.md)。

## 1. 先分清角色：你需要做什么

| 角色 | 要做的事 | 需要看哪里 |
| --- | --- | --- |
| **Extension 用户**（分析师 / 数据工程师） | 装 DuckDB + 装扩展 + 拿到 Gateway 地址 → 写 SQL | 本文 §2 |
| **平台 / 运维**（部署 Gateway 的人） | 部署 Gateway、连 HBase、配 TLS/认证、提供 endpoint | [部署指南](deployment-guide.md) |
| **开发者**（改本仓库代码） | 构建扩展、起本地 HBase/Gateway、跑测试 | 本文 §3、[环境说明](environment.md) |

终端用户**不需要**执行 `scripts/env-up.sh`、不需要装 HBase、不需要编译扩展；
`scripts/env-up.sh` 是本仓库开发者的“一键起本地联调环境”脚本。

## 2. 终端用户：三步查询

### 前提

| 项 | 说明 |
| --- | --- |
| DuckDB | 与扩展匹配的版本（当前基线 v2.0.0） |
| Extension | 已安装到 DuckDB（见第 1 步） |
| Gateway 地址 | 由运维提供，例如 `grpc://hbase-gw.internal:18080` |
| 权限 | 对目标 HBase namespace / 表的读权限 |

### 第 1 步：安装并加载扩展

扩展发布到 Extension Repository 后（M5 目标），用户侧是标准流程：

```sql
INSTALL hbase FROM community;
LOAD hbase;
```

当前 M1 阶段扩展尚未发布，使用运维/开发者提供的扩展文件：

```bash
duckdb -unsigned   # 本地构建产物未签名，必须加 -unsigned
```

```sql
LOAD '/opt/duckdb/extensions/hbase.duckdb_extension';   -- 必须是绝对路径
```

### 第 2 步：注册连接

```sql
SELECT hbase_connect('prod', 'grpc://127.0.0.1:18080');
-- true
```

`hbase_connect` 会做一次握手（Health + GetCapabilities）：Gateway 不可用时立即报错，
不会留下半可用状态。连接是**会话级**的，重开 CLI 需要重新 `hbase_connect`。

### 第 3 步：查询

```sql
SELECT "cf1:name", "cf2:age"::INTEGER AS age
FROM hbase_scan(
    'prod', 'duckdb_test', 'customer_profile',
    start_row := 'r001',
    stop_row  := 'r011',                    -- 开区间
    columns   := ['cf1:name','cf2:age']     -- 必须显式指定
)
WHERE "cf2:age"::INTEGER > 40;
```

## 3. 开发者 / 自建环境

只想在本机跑通链路时：

```bash
cd /path/to/duckdb-hbase

# 1) 拉起 HBase + 测试夹具 + Gateway（幂等）
bash scripts/env-up.sh

# 2) 构建扩展（首次约 15-30 分钟）
VCPKG_TOOLCHAIN_PATH="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" \
OVERRIDE_GIT_DESCRIBE=v2.0.0 GEN=ninja make release

# 3) 环境体检 / 停止
bash scripts/env-check.sh
bash scripts/env-down.sh
```

> `OVERRIDE_GIT_DESCRIBE=v2.0.0` 让扩展的 DuckDB 版本串与官方 v2.0.0 CLI 对齐；
> 不加会报 “built specifically for DuckDB version …”。

## 4. `hbase_scan` 参数

```sql
hbase_scan(
    connection_name,     -- 必填：hbase_connect 注册的名字
    namespace,           -- 必填：HBase namespace
    table_name,          -- 必填：表名
    start_row := NULL,   -- 起始 RowKey（含），NULL = 不限
    stop_row  := NULL,   -- 结束 RowKey（不含），NULL = 不限
    columns   := NULL,   -- 列列表 ['family:qualifier', ...]
    max_versions := 1,   -- >1 时按时间戳展开，并追加 __timestamp 列
    timestamp_min := NULL,
    timestamp_max := NULL,
    cache := NULL        -- 预留：M3 之前显式传 true 会报错
)
```

要点：

- **必须显式给 `columns`**，否则直接报错（M1 不做“退化为整列族”的静默降级）。HBase 没有固定 schema，Gateway 只能拿到列族。
- 列名一律用 `"family:qualifier"` 双引号引用（因为含冒号）。
- 所有列当前都是 `VARCHAR`（类型映射待定），数值需要 `CAST`。

## 5. 常用示例

```sql
-- 计行数
SELECT count(*) FROM hbase_scan('prod','duckdb_test','customer_profile',
                                columns := ['cf1:name']);

-- 聚合
SELECT "cf1:city", count(*) AS n
FROM hbase_scan('prod','duckdb_test','customer_profile', columns := ['cf1:city'])
GROUP BY 1 ORDER BY 2 DESC LIMIT 10;

-- 范围 + 过滤
SELECT "cf1:name", "cf2:age"::INTEGER AS age
FROM hbase_scan('prod','duckdb_test','customer_profile',
                start_row := 'r001', stop_row := 'r050',
                columns := ['cf1:name','cf2:age'])
WHERE "cf2:age"::INTEGER > 40;

-- 时间戳过滤
SELECT * FROM hbase_scan('prod','duckdb_test','customer_profile',
                         columns := ['cf2:score'], timestamp_min := 1000, timestamp_max := 2000);

-- 断开连接
SELECT hbase_disconnect('prod');   -- true 表示删除成功
```

## 6. 脚本 / CI 方式

```bash
duckdb -unsigned -c "
LOAD '$(pwd)/build/release/extension/hbase/hbase.duckdb_extension';
SELECT hbase_connect('prod','grpc://127.0.0.1:18080');
SELECT count(*) FROM hbase_scan('prod','duckdb_test','customer_profile', columns := ['cf1:name']);
"
```

## 7. 已知限制（M1）

| 限制 | 说明 / 规避 |
| --- | --- |
| 扩展未发布 | 需要 `LOAD` 本地文件 + `-unsigned`；发布到 community extensions 是 M5 |
| 依赖 Gateway | Extension 不直连 HBase，必须由运维提供 Gateway 端点 |
| 缓存未实现 | 显式传 `cache := true` 会报错 `缓存引擎尚未实现（M3）`；省略即可正常查询 |
| 只有字符串类型 | 数值/时间需要 `CAST` |
| 单线程扫描 | 一个查询一个远端流；并行扫描在 M2 |
| 无 Catalog | 必须显式传 namespace / table；不支持 `SELECT *`（必须给 columns） |
| 只读 | 不支持写入、DDL |
| 多版本 | `max_versions > 1` 时按 Cell 时间戳展开成多行，并追加 `__timestamp`（VARCHAR，毫秒） |
| 按列扫描跳过无匹配行 | 只请求 `ext:remark` 时，remark 缺失的行不会返回；需要 NULL 语义时同时请求一个必然存在的列，或先 `CREATE TABLE t AS SELECT ...` |
| `IS NULL` / `FILTER` 不稳定 | 先 `CREATE TABLE t AS SELECT ...` 再查 `t`（已验证语义正确） |

## 8. 排错

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `dlopen(... relative path not allowed ...)` | macOS 安全策略 | `LOAD` 用绝对路径 |
| `signature is either missing or invalid` | 扩展未签名 | `duckdb -unsigned` |
| `built specifically for DuckDB version ...` | 扩展与 CLI 版本不一致 | 用同一 v2.0.0 构建，构建时加 `OVERRIDE_GIT_DESCRIBE=v2.0.0` |
| `hbase_connect: Gateway 不可用` | Gateway 未启动 / 地址错 | 联系运维确认 endpoint |
| 查询返回 0 行 | 范围写错（`stop_row` 开区间）/ 只请求了缺失列 | 检查 RowKey 前缀；同时请求基础列 |
| `hbase_scan: 连接 'xxx' 未注册` | 没执行 `hbase_connect` | 先注册连接 |
