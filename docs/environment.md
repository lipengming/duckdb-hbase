# 开发环境安装与使用说明

> 面向本机开发与 M1 集成测试。本文是**本机开发环境**的唯一说明文档；生产/集群部署见 [部署指南](deployment-guide.md)。
>
> - 安装脚本：[`scripts/install-dev-env.sh`](../scripts/install-dev-env.sh)（幂等，可重复执行）
> - 环境自检：[`scripts/check-env.sh`](../scripts/check-env.sh)
> - 测试夹具：[`scripts/hbase-fixture.sh`](../scripts/hbase-fixture.sh)
>
> 已验证平台：macOS 15.8.1（Intel x86_64，16GB 内存）。Apple Silicon 走同一脚本（自动切换 `arm64` 产物）。

## 1. 我要做什么（速查）

| 目的 | 命令 |
| --- | --- |
| 全新安装四件套 | `bash scripts/install-dev-env.sh` |
| 只装到自定义目录、不碰 `/usr/local/bin` | `bash scripts/install-dev-env.sh --prefix ~/opt/dh --no-global` |
| 启动 HBase | `start-hbase.sh` |
| 停止 HBase | `stop-hbase.sh` |
| 确认环境可用 | `bash scripts/check-env.sh` |
| 建测试表与数据 | `bash scripts/hbase-fixture.sh` |
| 查 HBase 状态 | `printf "status 'summary'\n" \| hbase shell -n` |
| 打开 DuckDB | `duckdb` |

## 2. 版本矩阵

四个组件版本都是**硬约束**，不要自行升级：

| 组件 | 版本 | 安装位置 | 为什么必须是这个版本 | 验证 |
| --- | --- | --- | --- | --- |
| JDK | Temurin **11.0.32.1** | `~/tools/jdk11` | Gateway 基线为 JDK 11；HBase 2.6.x 官方只支持 8/11 | `java -version` → `11.0.32.1` |
| Maven | **3.9.16** | `~/tools/apache-maven-3.9.16` | 3.9.x 是仍支持 JDK 8+ 的稳定线，要求 ≥ 3.8 | `mvn -v` |
| HBase | **2.6.6**（原生 standalone） | `~/tools/hbase-2.6.6` | 设计基线指定的 2.6.x 系列；扫描语义/客户端 API 依赖它 | `hbase version` → `HBase 2.6.6` |
| DuckDB CLI | **2.0.0** | `~/tools/bin/duckdb` | 必须与 `duckdb/` submodule 及扩展构建基线一致，否则 `LOAD` 因 ABI/版本校验失败 | `duckdb --version` → `v2.0.0` |

安装体积：下载约 570MB，装完约 805MB（下载缓存会自动清理）。

## 3. 一键安装

```bash
git clone --recurse-submodules git@github.com:lipengming/duckdb-hbase.git
cd duckdb-hbase
bash scripts/install-dev-env.sh
```

脚本参数：

| 参数 | 作用 |
| --- | --- |
| 无参数 | 装到 `~/tools`；写 `/usr/local/bin` 入口；写 `~/.zshrc`；注册 JDK 到 `~/Library/Java/JavaVirtualMachines` |
| `--prefix DIR` / `DEV_PREFIX=DIR` | 换安装前缀（默认 `~/tools`） |
| `--no-global` | 只装到前缀，不动 `/usr/local/bin`、`~/.zshrc`、`~/Library/Java`（CI 或无权限时用） |
| `-h` | 打印用法 |

行为说明：

- **幂等**：已完整下载的包（`gzip -t` 校验通过）跳过下载；已解压的组件跳过解压；HBase 配置块与 `~/.zshrc` 块按标记覆盖，不会重复追加。
- **不会因单步失败而中断**：失败项计入末尾汇总，最后以非 0 退出码结束（便于一次性暴露所有网络/权限问题）。
- **不需要 sudo**：全部写在用户目录。唯一例外是 `/usr/local/bin`，若不可写则跳过并提示。
- **HBase 下载走华为云镜像**：官方 `archive.apache.org` 实测约 160KB/s（341MB 需 35 分钟），镜像约 8MB/s；镜像失败自动回退官方源。

安装完成后脚本会直接打印四个组件的版本号作为自验。

## 4. 安装了什么

### 4.1 目录布局

```text
~/tools/
├── jdk11 -> jdks/jdk-11.0.32.1+1/Contents/Home   # JAVA_HOME 指向这里
├── jdks/jdk-11.0.32.1+1/                          # bundle 根（含 Contents/Home）
├── apache-maven-3.9.16/
├── hbase-2.6.6/
│   └── conf/
│       ├── hbase-site.xml    # 数据目录固定到 ~/tools/hbase-data（脚本生成）
│       └── hbase-env.sh      # JAVA_HOME + JDK 11 的 --add-opens（脚本生成，标记块）
├── hbase-data/               # HBase 数据 + 内置 ZooKeeper + 日志
│   ├── logs/
│   └── zookeeper/
└── bin/duckdb
```

### 4.2 三个额外的全局入口

| 位置 | 内容 | 作用 |
| --- | --- | --- |
| `/usr/local/bin/{mvn,hbase,start-hbase.sh,stop-hbase.sh}` | 包装脚本（内含 `JAVA_HOME`/`HBASE_HOME`） | **非交互 shell / CI 脚本**不加载 `~/.zshrc`，靠它才能直接用 |
| `/usr/local/bin/duckdb` | 软链 → `~/tools/bin/duckdb` | 同上；不需要环境变量 |
| `~/Library/Java/JavaVirtualMachines/temurin-11.jdk` | 软链 → JDK bundle 根 | `/usr/bin/java` 与 `/usr/libexec/java_home -v 11` 因此能发现 JDK 11 |
| `~/.zshrc` 中 `duckdb-hbase toolchain` 块 | `JAVA_HOME` / `MAVEN_HOME` / `HBASE_HOME` / `PATH` | **交互式 zsh** 中直接生效 |

## 5. 关键设计决策

### 5.1 为什么不用 Homebrew

| 需求 | Homebrew 的问题 |
| --- | --- |
| 只装 JDK 11 | `brew install maven` 声明依赖 `openjdk`（**最新版**，非 11），会顺带装出第二个 JDK，破坏"只装 JDK 11" |
| HBase 2.6.6 | `brew` 的 `hbase` 公式已是 **3.0.0**，版本不符 |
| 可复现 | tarball + 固定版本号 + 校验，比公式滚动更可控；也不需要 sudo |

结论：四个组件一律用官方二进制 tarball 装到用户目录。

### 5.2 为什么是"包装脚本"而不是软链

`start-hbase.sh` / `stop-hbase.sh` / `hbase` 用 `dirname $0` 定位 `HBASE_HOME`。如果软链到 `/usr/local/bin`，脚本会去 `/usr/local/bin/conf` 找配置而失败。因此这些命令用包装脚本 `exec` 真实路径。

### 5.3 JDK 11 的 `--add-opens`

HBase 2.6 在 JDK 11 上需要放开 JDK 内部包（`--illegal-access=permit` 在 JDK 11 后已不可用）。网上流传的完整清单里混入了 JDK 11 中**已拆成独立 module** 的包（如 `java.security.sasl`、`java.security.jgss`），照抄会得到一堆噪音：

```text
WARNING: package java.security.sasl not in java.base
```

脚本里的清单是逐条用 `java --add-opens=... -version` 验证过的，只保留真实存在的 14 条，`hbase version` 输出零警告。写在 `hbase-env.sh` 的 `# >>> DuckDB-HBase local dev >>>` 块中，可重复覆盖。

### 5.4 HBase 数据目录

固定在 `~/tools/hbase-data`（而不是默认的 `/tmp/hbase-*`），避免重启后被系统清理导致元数据与 ZooKeeper 状态一起丢失。

## 6. 使用方法

### 6.1 JDK 11

```bash
java -version                      # openjdk version "11.0.32.1"
javac -version                     # javac 11.0.32.1
echo "$JAVA_HOME"                  # 交互式 shell 中：/Users/<you>/tools/jdk11
/usr/libexec/java_home -v 11       # 打印 .../jdk-11.0.32.1+1/Contents/Home
```

- `JAVA_HOME` 必须指向 **`Contents/Home`**，不是 JDK 的 bundle 根目录（写错会得到 `mvn: The JAVA_HOME environment variable is not defined correctly`）。
- 只有 JDK 11 一个版本，`java` 不会有多版本歧义。
- 退出码/参数等属标准用法，无"启动/停止"概念。

### 6.2 Maven

```bash
mvn -v                                              # 确认 Maven 3.9.16 + Java 11.0.32.1
mvn -q -f gateway/pom.xml -DskipTests package       # 构建 Gateway（产出 gateway/*/target/）
mvn -q -f gateway/pom.xml test                      # 跑 Gateway 单测
```

- 首次构建需联网下载依赖到 `~/.m2/repository`，之后可离线增量构建。
- 包装脚本已固定 `JAVA_HOME=~/tools/jdk11`；若临时要用别的 JDK：`JAVA_HOME=/path/to/jdk mvn ...`。
- Maven 是命令行工具，无"启动/停止"概念。

### 6.3 HBase（standalone）—— 启动与停止

```bash
start-hbase.sh        # 启动：Master + RegionServer + 内置 ZooKeeper，实测约 10 秒
stop-hbase.sh         # 停止（异步，端口释放通常需 10~20 秒）
```

验证是否起来：

```bash
jps                                        # 应看到 HMaster（standalone 下 RegionServer 内嵌）
printf "status 'summary'\n" | hbase shell -n
# 期望：1 active master, 0 backup masters, 1 servers, 0 dead
lsof -nP -iTCP -sTCP:LISTEN | grep -E ':(2181|16000|16010)'
```

端口与界面：

| 端口 | 用途 | 备注 |
| --- | --- | --- |
| 2181 | ZooKeeper | 内置，`HBASE_MANAGES_ZK=true` |
| 16000 | Master RPC | |
| 16010 | Master Web UI | <http://localhost:16010> |
| 16020 | RegionServer RPC | standalone 下与 master 同进程，实测**不一定**监听；`check-env.sh` 因此会保留一条 WARN，属正常 |

日志与数据：

```bash
ls ~/tools/hbase-data/logs/                # master/regionserver 的 .log 与 .out
tail -f ~/tools/hbase-data/logs/hbase-*-master-*.log
```

常用 shell 操作（`hbase shell` 交互式，或 `-n` + 管道非交互）：

```bash
printf "list\n" | hbase shell -n
printf "count 'duckdb_test:customer_profile'\n" | hbase shell -n
printf "get 'duckdb_test:customer_profile', 'r0011', {COLUMN => 'cf2:score', VERSIONS => 3}\n" | hbase shell -n
```

**重置数据**（清空所有表，重新开始）：

```bash
stop-hbase.sh
sleep 15                                    # 等进程退出、端口释放
rm -rf ~/tools/hbase-data/{data,MasterProcWALs,WALs,oldWALs,zookeeper,tmp}
start-hbase.sh
bash scripts/hbase-fixture.sh               # 重建夹具
```

内存（16GB 机器默认 1GB 堆够用，可按需改 `~/tools/hbase-2.6.6/conf/hbase-env.sh`）：

```bash
export HBASE_HEAPSIZE=2048                  # Master 堆（MB）
export HBASE_REGIONSERVER_OPTS="-Xmx2048m"  # RegionServer 堆
```

### 6.4 DuckDB CLI

```bash
duckdb                                      # 交互式（内存库）
duckdb my.db                                # 打开/创建数据库文件
duckdb -c "SELECT 42"                       # 单条命令
duckdb -csv -c "SELECT * FROM range(3)"     # 切换输出格式（-csv / -json / -markdown / -box）
duckdb -f examples/query.sql                # 执行 SQL 文件
```

验证本仓库扩展（**本地未签名扩展必须加 `-unsigned`**）：

```bash
./build/release/duckdb -unsigned -c \
  "LOAD 'build/release/extension/hbase/hbase.duckdb_extension'; SELECT hbase_version();"
```

> 用 `~/tools/bin/duckdb`（或 `/usr/local/bin/duckdb`）验证扩展也可以，但版本必须同为 `v2.0.0`，否则 `LOAD` 失败。

### 6.5 典型完整流程

```bash
# 1. 环境
bash scripts/check-env.sh

# 2. HBase 起来并建夹具
start-hbase.sh
bash scripts/hbase-fixture.sh

# 3. 构建 Extension（首次约 15-30 分钟）
GEN=ninja EXT_FLAGS="-DCMAKE_POLICY_VERSION_MINIMUM=3.5" make release

# 4. 单测 + 手工验证
./build/release/test/unittest "*hbase*"
./build/release/duckdb -unsigned -c \
  "LOAD 'build/release/extension/hbase/hbase.duckdb_extension'; SELECT hbase_version();"

# 5. 收工
stop-hbase.sh
```

## 7. 测试夹具

环境起来后执行一次（可重复，默认重建表）：

```bash
bash scripts/hbase-fixture.sh
```

| 表 | 内容 | 用途 |
| --- | --- | --- |
| `duckdb_test:customer_profile` | 100 行，`cf1`（name/city/phone）、`cf2`（age/score），3 个版本 | 投影、过滤、范围、缓存命中 |
| 其中 `r0007` | 缺 `cf1:phone` | NULL / 缺列语义 |
| 其中 `r0011` | `cf2:score` 有 ts=1000/2000 两版 | 版本与时间戳过滤 |
| `duckdb_test:edge_cases` | 二进制 RowKey、中文 RowKey、空值、64KB 长值、200 列宽行 | RowKey 字节序、类型映射、宽行 |

## 8. 环境自检

```bash
bash scripts/check-env.sh          # 加 --strict 则 WARN 也算失败（CI 用）
```

HBase 未启动时的预期基线（本机实测）：

```text
PASS=13  WARN=6  FAIL=0
```

WARN 全部可解释：

| WARN | 原因 | 是否要处理 |
| --- | --- | --- |
| 未设置 `JAVA_HOME` | 仅出现在**非交互** shell；包装脚本内部已设 | 否 |
| HBase 集群未连接 + 2181/16000/16010 未监听 | HBase 没启动 | 需要时 `start-hbase.sh` |
| 16020 未监听 | standalone 下 RegionServer 内嵌 | 否（启用了也常驻 WARN） |

**判据：`FAIL=0` 即可开工。**

## 9. 常见问题

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `mvn: The JAVA_HOME environment variable is not defined correctly` | `JAVA_HOME` 指向 bundle 根而非 `Contents/Home` | `export JAVA_HOME=~/tools/jdk11`（软链已指向 `Contents/Home`） |
| `WARNING: package java.security.sasl not in java.base` | `--add-opens` 清单含 JDK 11 已拆分的包 | 用本仓库脚本生成的清单（已剔除） |
| `archive.apache.org` 下载只有 ~160KB/s | 官方归档站限速 | 脚本已优先走华为云镜像 |
| `dv（: unbound variable` 之类 | macOS 自带 bash 3.2 把全角字符并入变量名 | 变量写成 `${dv}`（`check-env.sh` 已修） |
| `timeout: command not found` | macOS 无 GNU `timeout` | `brew install coreutils` 或脚本内用 `tmo` 兜底 |
| HBase 起不来 / 立刻退出 | 看 `~/tools/hbase-data/logs/*.out`；多数是 `JAVA_HOME` 不对或上次未正常停止 | 修 `JAVA_HOME`；必要时按 6.3「重置数据」 |
| 端口被占（2181/16000/16010） | 残留 HBase 进程 | `lsof -nP -iTCP:2181 -sTCP:LISTEN` 找到 PID 后 `kill` |
| `LOAD ... duckdb_extension` 失败 | DuckDB CLI 版本不是 v2.0.0，或忘记 `-unsigned` | 用 `~/tools/bin/duckdb`；手工验证加 `-unsigned` |
| 装完新终端仍找不到 `mvn` | 当前 shell 未重新加载配置 | 新开终端或 `source ~/.zshrc`；非交互场景走 `/usr/local/bin` 包装脚本 |

## 10. 升级 / 重置 / 卸载

**升级单个组件**：改 `scripts/install-dev-env.sh` 顶部的 `JDK_VER` / `MVN_VER` / `HBASE_VER` / `DUCK_VER`，删掉对应旧目录后重跑（目录存在即跳过，这是幂等的前提），注意同步更新第 2 节版本矩阵与 `duckdb/` submodule。

**完全卸载**：

```bash
stop-hbase.sh
rm -f /usr/local/bin/{mvn,hbase,start-hbase.sh,stop-hbase.sh,duckdb}
rm -f ~/Library/Java/JavaVirtualMachines/temurin-11.jdk
# 删除 ~/.zshrc 中 # >>> duckdb-hbase toolchain >>> 到 # <<< 之间的块
rm -rf ~/tools
```

## 11. 待定（TBD）

| 项 | 需要的输入 |
| --- | --- |
| Linux / CI 环境安装 | 目标发行版与是否允许 sudo；脚本目前只支持 macOS |
| Windows（WSL2 或原生） | 是否有使用方 |
| Python `duckdb` 包（`pip install duckdb==2.0.0`） | M2 Python 示例需要时再装，当前只装了 CLI |
| HBase REST / Thrift 服务（`hbase rest start` / `hbase thrift start`） | M5 `adapter-rest` / `adapter-thrift` 需要时启用 |
| 多版本 HBase 兼容矩阵（2.4.x 等） | M5 引入 Docker 后统一管理 |

## 12. 不需要准备的东西

| 项 | 说明 |
| --- | --- |
| DuckDB 源码 | `git submodule`（`duckdb/`，固定 v2.0.0） |
| extension-ci-tools | `git submodule`（`extension-ci-tools/`） |
| vcpkg 与 C++ 依赖（gRPC / Protobuf / OpenSSL） | 由 extension-ci-tools 的 Makefile 自动引导 |
| protoc / Arrow | 构建时由 vcpkg 与 DuckDB 内置提供，不手工安装 |
| Gateway 可执行包 | 由 `gateway/` 的 Maven 构建产出 |
| cmake / ninja / python3 / git / C++ 编译器 | 本机已具备（见 `check-env.sh` 第 1、2 节） |
| 测试 SQL / 夹具脚本 | 已在 `scripts/` 与 `test/` 中维护 |
