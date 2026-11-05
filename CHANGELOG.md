# Changelog

本文件记录本仓库的显著变更。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [Unreleased]

### Added

- **M3 缓存闭环**：`CacheManager` 门面 + 覆盖索引 + 段文件存储 + SQLite 元数据 + Full Hit 读取路径。
  - 覆盖判定按 HBase 字节序（`memcmp` 语义）比较，五项条件（范围、列集合、时间戳语义、未过期、COMMITTED 状态）全满足才算 Full Hit；段范围必须是请求范围的子集，避免读出越界行。M3 不做 Partial Hit，任何缺口/缺列一律回源。
  - `hbase_scan` 命中路径完全不访问 Gateway；未命中路径边返回边 tee 写段，分区正常结束才 `Finish`（rename 发布 + 单事务提交元数据），`LIMIT`/取消/异常/析构时 `Abort`，不会提交截断结果。
  - `hbase_scan(..., cache := true)` 默认开启缓存；`max_versions > 1`、指定时间戳、`stop_row` 闭区间一律绕过缓存且不写缓存。
  - 配置项：`hbase_cache_enabled` / `hbase_cache_ttl` / `hbase_cache_directory` / `hbase_cache_mode`（`file`/`memory`）/ `hbase_cache_max_size`（仅登记）。
  - 新增 `hbase_cache_stats()`（11 列，`docs/cache-design.md` §9.1 定稿）与 `hbase_cache_clear()` 的 0/2/3 参数三种签名。
  - 崩溃恢复：`staging/*` 残留与元数据中非 `COMMITTED` 记录在首次使用时清理；SQLite 用 WAL + `synchronous=FULL`。
- **C++ 单元测试**：`extension/test/unit/`（Catch2 单头文件，来自 DuckDB `third_party/catch`，无新依赖），覆盖覆盖索引、段文件往返与损坏检测、元数据原子提交与崩溃残留、`CacheManager` 命中/TTL/失效/统计/memory 模式；`make unit` 运行。
- **构建**：vcpkg 增加 `sqlite3`（缓存元数据）。

### Changed

- `hbase_scan` 的 `cache` 参数语义从"M1 未实现，显式 true 报错"改为"默认 true，可正常使用缓存"。
- **ADR-002**：M3 段数据格式采用自研列式段文件 `DHBSEG01`，Parquet 延后（DuckDB 未把 in-tree parquet 链接进 `libduckdb`，loadable extension 无法直接使用；`-force_load` 会因第三方符号重复实例化崩溃）；`hbase_cache_mode='parquet'` 显式报错。
- **ADR-003**：元数据表用 `namespace` + `table_name` 替代 `table_id`，`status` 增加 `INVALID`。

### Added（历史条目）

- **多版本展开**：`max_versions > 1` 时 Gateway 按 Cell 时间戳展开成多行，追加 `__timestamp` 列；集成测试覆盖 r011 双版本。
- **并行扫描**：按 RowKey 区间切分为多个分区，每个线程持有独立远端流（`MaxThreads = 分区数`）；分区数受 Gateway `max_parallel_scans` 约束。
- **ADR-001**：记录 M2 数据面使用 UTF8 列式编码、Arrow IPC 延后的决策与回退条件。
- **M2 协议与错误模型**：`ErrorInfo`（code/message/retryable/request_id）+ trailing metadata `hbase-error-bin`；Gateway 统一错误分类，Extension 还原为可诊断信息；幂等 RPC 自动重试一次。
- **取消链路**：`Provider.Cancel` + 查询中断 / LIMIT 提前结束时关闭远端 Scanner；Gateway `Cancel` 按 request_id 幂等关闭游标。
- **能力协商**：连接握手结果缓存到连接表，`hbase_scan` 在 `supports_scan=false` 时直接拒绝；新增协议字段稳定性测试。
- **显式失败优于静默降级**：省略 `columns`、`max_versions > 1`、显式 `cache := true` 现在都会给出明确错误（M1 未实现的能力不再“看起来能跑”）。
- **数据面（Arrow IPC）**：Gateway 侧 `ArrowIpcEncoder`（每批一条完整 IPC stream）+ `Scan`/`BatchGet` 流式返回；Extension 侧 `RemoteScanStream` 后台拉取 + `DecodeArrowIpc` 解码 + DataChunk 填充。
- **集成测试**：`integration/`（1M 行客户交易宽表 mock、HBase 装载、pytest 端到端用例）。
- **Extension 远端 Provider**：`IRemoteScanProvider` 抽象、gRPC 客户端（Health / GetCapabilities / GetSchema）、`hbase_connect` 建连即握手（失败不注册连接）、`hbase_scan` 从 Gateway 取 Schema 完成 Bind；数据面 Arrow IPC 待实现。
- **构建**：vcpkg 引入 gRPC/Protobuf，CMake 自动从 `protocol/proto` 生成 C++ 桩。
- **统一环境脚本**：`scripts/env-up.sh`（HBase + 夹具 + Gateway 一键启动，幂等）、`scripts/env-down.sh`（停止，可 `--keep-hbase`）、`scripts/env-check.sh`（只读体检：工具链 / HBase / 夹具 / Gateway / Extension）；`check-env.sh` 保留为兼容入口。
- **本机验证**：Gateway 集成测试（Native Adapter 直连 HBase、进程内 gRPC Server）共 11 项全部通过。
- **集成测试包**（`integration/`，独立 Python 包，不参与 Extension/Gateway 构建）：确定性中文「客户交易宽表」mock 数据（100 万行，单客户 10 万/50 万/100 万三档，30 天跨度，RowKey=`<salt><user_id>#<yyyymmdd>#<seq>`），HBase 16 预分区建表与 BufferedMutator 批量装载（1M 行实测 42s），manifest 抽样校验，以及数据面 / Extension / 缓存 / 规模 / 错误路径五组 pytest 用例；环境缺失或 `hbase_scan` 数据面未实现时自动 skip。
- **文档**：`docs/testing.md` 新增 §5.2 客户交易宽表与 §5.4 可执行集成测试包、用例编号与执行约定；`docs/environment.md` 新增 §7.1 规模夹具用法；`integration/README.md` 记录数据模型、三档切换、CLI、环境变量与已知限制。
- **M1 Gateway 骨架**：Maven 多模块（`gateway-api` / `gateway-core` / `adapter-native` / `gateway-server`）、`HBaseAccessAdapter` SPI + `ServiceLoader` 装配、Native HBase Adapter（Scan / BatchGet / 表结构 / 取消 / 健康检查）、gRPC 服务（Health / GetCapabilities / GetSchema / Cancel 已接通，Scan 与 BatchGet 待 Arrow IPC）。
- **协议补充**：`CapabilityResponse`（11 项能力）、`SchemaRequest` / `ColumnSchema` / `SchemaResponse`、`CancelRequest` / `CancelResponse`、`HealthRequest` / `HealthResponse` 字段定稿（草案），并开启 `java_multiple_files`。
- **文档**：`docs/gateway-design.md` 增加 M1 实现状态与包名偏差说明；`docs/protocol.md` 补齐新增消息字段表。

- **M1 Extension 骨架**：`hbase_version()`、`hbase_connect()` / `hbase_disconnect()`（端点解析、会话级连接表）、`hbase_scan()` 参数解析与校验（远端 Provider 未接入前显式报错）。
- **构建体系**：`CMakeLists.txt`、`Makefile`、`extension_config.cmake`、`vcpkg.json`；`duckdb`（v2.0.0）与 `extension-ci-tools` submodule。
- **测试**：`extension/test/sql/hbase_basic.test`（注册、幂等覆盖、非法端点、未注册连接、非法列描述等 10 个断言）。
- **环境工具**：`scripts/install-dev-env.sh`（一键安装 JDK 11 / Maven 3.9.16 / HBase 2.6.6 / DuckDB CLI 2.0.0，用户级、幂等、无需 sudo）、`scripts/check-env.sh`（工具链/HBase 自检）、`scripts/hbase-fixture.sh`（建表与夹具数据）、`docs/environment.md`（开发环境安装与使用说明）。

- 仓库工程骨架：`docs/` 文档体系、`extension/` `gateway/` `protocol/` `scripts/` `examples/` 目录说明。
- AI 编码指南 `AGENTS.md`（含架构硬约束、阅读顺序、文档同步矩阵）与 `CLAUDE.md` 入口。
- 协作与规范文档：`CONTRIBUTING.md`、`docs/coding-standards.md`。
- 设计文档族：`architecture.md`、`extension-design.md`、`gateway-design.md`、`protocol.md`、`cache-design.md`、`user-guide.md`、`deployment-guide.md`、`testing.md`、`roadmap.md`。
- 协议草案 `protocol/proto/hbase_data_service.proto`。
- 工程配置：`.editorconfig`、`.clang-format`、`.gitattributes`、`.gitignore`、Issue/PR 模板。

## [0.1.0] - 2026-11-01

### Added

- 技术方案 V1.0（`tech_v0.1.md`）：架构分层、Extension 模块设计、Gateway 抽象与多接入适配、缓存引擎、用户 API、安装部署、分包设计、里程碑规划。
