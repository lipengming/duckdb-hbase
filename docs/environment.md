# 测试环境准备清单

> 面向 M1（Extension + Native Gateway + 真实 HBase Scan）。标 **必须** 的项缺失会阻塞集成测试；标 **建议/可选** 的项可后补。
> 本机现状（已探测）：macOS 15.8.1 / Intel x86_64 / 12 核 / 16GB 内存 / 312GB 可用磁盘；已有 cmake 4.4.4、ninja、Apple clang 17、python3、git、uv。

## 1. 必须准备

| 组件 | 版本 | 用途 | 验证命令 |
| --- | --- | --- | --- |
| JDK | Temurin 11（LTS） | Gateway 运行时；HBase 2.6.x 官方支持 8/11 | `java -version` 显示 11.x |
| Maven | 3.9.x | Gateway 多模块构建 | `mvn -version` |
| HBase | 2.6.6（standalone 即可） | 集成测试数据源 | `hbase shell` 能执行 `status` |
| DuckDB CLI | v2.0.0 | `LOAD` 扩展、执行 SQL 验证 | `duckdb --version` |
| 磁盘 | ≥ 20GB 可用 | DuckDB 源码 + vcpkg 依赖（gRPC/Protobuf）构建 | `df -h /` |
| 网络 | 可访问 GitHub / Maven Central | 拉取 submodule、vcpkg 依赖、Maven 依赖 | `curl -I https://github.com` |

### 1.1 安装 JDK 11 与 Maven

```bash
brew install openjdk@11 maven
# 让系统 java 指向 11（按需选择其一）
export JAVA_HOME="$(brew --prefix openjdk@11)/libexec/openjdk.jdk/Contents/Home"
export PATH="$JAVA_HOME/bin:$PATH"
java -version   # 期望 11.x
```

> 把 `JAVA_HOME` / `PATH` 写入 `~/.zshrc`，HBase 的启动脚本依赖 `JAVA_HOME`。

### 1.2 安装 HBase 2.6.6（standalone）

```bash
mkdir -p ~/opt && cd ~/opt
curl -LO https://archive.apache.org/dist/hbase/2.6.6/hbase-2.6.6-bin.tar.gz
tar -xzf hbase-2.6.6-bin.tar.gz
export HBASE_HOME="$HOME/opt/hbase-2.6.6"
export PATH="$HBASE_HOME/bin:$PATH"
```

在 `$HBASE_HOME/conf/hbase-site.xml` 中固定数据目录（避免 `/tmp` 被系统清理）：

```xml
<configuration>
  <property>
    <name>hbase.rootdir</name>
    <value>file:///Users/cubber.li/hbase-data</value>
  </property>
  <property>
    <name>hbase.zookeeper.property.dataDir</name>
    <value>/Users/cubber.li/hbase-data/zookeeper</value>
  </property>
</configuration>
```

启动与验证：

```bash
start-hbase.sh          # standalone：Master + RegionServer + 内置 ZooKeeper 同进程
jps                     # 应看到 HMaster / HRegionServer
hbase shell -n <<< "status 'summary'"
# Web UI: http://localhost:16010
```

停止：`stop-hbase.sh`。

### 1.3 安装 DuckDB CLI v2.0.0

```bash
curl -LO https://github.com/duckdb/duckdb/releases/download/v2.0.0/duckdb_cli-osx-amd64.zip
unzip duckdb_cli-osx-amd64.zip -d ~/opt/duckdb
export PATH="$HOME/opt/duckdb:$PATH"
duckdb --version   # v2.0.0
```

> 版本必须与 Extension 构建基线一致（v2.0.0），否则 `LOAD` 会因 ABI/版本校验失败。

## 2. 测试数据夹具

环境起来后执行一次（可重复执行，默认重建表）：

```bash
cd /Users/cubber.li/code/duckdb-hbase
bash scripts/hbase-fixture.sh
```

生成内容：

| 表 | 内容 | 用途 |
| --- | --- | --- |
| `duckdb_test:customer_profile` | 100 行，列族 `cf1`（name / city / phone）、`cf2`（age / score），3 个版本 | 投影、过滤、范围、缓存命中 |
| 其中 `r0007` | 缺 `cf1:phone` | NULL / 缺列语义 |
| 其中 `r0011` | `cf2:score` 有 ts=1000 / 2000 两个版本 | 版本与时间戳过滤 |
| `duckdb_test:edge_cases` | 二进制 RowKey、中文 RowKey、空值、64KB 长值、200 列宽行 | RowKey 字节序、类型映射、宽行 |

## 3. 一键自检

```bash
bash scripts/check-env.sh
```

脚本会检查工具链、JDK/Maven、DuckDB 版本、HBase 连通性与端口、磁盘内存，并输出 PASS / WARN / FAIL 汇总。所有 FAIL 清零即可开始集成测试。

## 4. 你**不需要**准备的东西

以下由我在仓库内自动化处理，避免重复劳动：

| 项 | 说明 |
| --- | --- |
| DuckDB 源码 | `git submodule`（`duckdb/`，固定 v2.0.0） |
| extension-ci-tools | `git submodule`（`extension-ci-tools/`） |
| vcpkg 与 C++ 依赖（gRPC / Protobuf / OpenSSL） | 由 extension-ci-tools 的 Makefile 自动引导 |
| protoc / Arrow | 构建时由 vcpkg 与 DuckDB 内置提供，不手工安装 |
| Gateway 可执行包 | 由 `gateway/` 的 Maven 构建产出 |
| 测试 SQL / 夹具脚本 | 已在 `scripts/` 与 `test/` 中维护 |

## 5. 可选（后续里程碑，现在不急）

| 组件 | 需要的里程碑 | 说明 |
| --- | --- | --- |
| Docker Desktop | M5 | 多版本 HBase 兼容矩阵、CI 集成测试 |
| HBase REST / Thrift 服务 | M5 | `adapter-rest` / `adapter-thrift` 验证（`hbase rest start` / `hbase thrift start`） |
| 第二套 HBase 版本（如 2.4.x） | M5 | 版本兼容性验证 |
| Kerberos KDC / keytab | M5 | `adapter-native` 安全认证 |
| Python 环境（`pip install duckdb==2.0.0`） | M2 | Python API 示例验证（也可用 `uv`） |

## 6. 环境变量约定

```bash
export JAVA_HOME="$(brew --prefix openjdk@11)/libexec/openjdk.jdk/Contents/Home"
export HBASE_HOME="$HOME/opt/hbase-2.6.6"
export PATH="$JAVA_HOME/bin:$HBASE_HOME/bin:$HOME/opt/duckdb:$PATH"
```

| 变量 | 使用者 | 说明 |
| --- | --- | --- |
| `JAVA_HOME` | HBase、Maven | JDK 11 根目录 |
| `HBASE_HOME` | HBase 脚本、集成测试 | HBase 安装目录 |
| `HBASE_CONF_DIR` | 集成测试 | 可选，默认 `$HBASE_HOME/conf` |
| `DUCKDB_BIN` | 测试脚本 | 可选，默认 `duckdb` |

## 7. 我这边并行推进的工作（不依赖上述环境）

1. ~~Extension 骨架~~（已完成）：CMake 构建、`hbase_version` / `hbase_connect` / `hbase_disconnect`、`hbase_scan` 参数解析与连接校验，SQLLogicTest 通过。
2. Gateway 骨架：Maven 多模块 + gRPC 服务端 + Native Adapter 读取 `duckdb_test` 表。
3. 单元测试：Provider 用 Mock，覆盖 Scan Plan、投影、RowKey 范围。
4. CI：GitHub Actions 上的格式检查与编译（不含 HBase）。

环境就绪后，我会用 `scripts/check-env.sh` 与 `scripts/hbase-fixture.sh` 做真实链路验证，并把结果写回 `docs/roadmap.md` 的 M1 勾选项。
