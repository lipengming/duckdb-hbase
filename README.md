# DuckDB-HBase

> 一个支持多种 HBase 接入协议、具备本地列式 Scan Cache 的 DuckDB 外部数据源扩展。

DuckDB-HBase 不只是一个 SQL Connector，而是一个具备多接入适配能力和本地列式 Scan Cache 的 HBase 分析型数据访问引擎：用户仅通过 DuckDB SQL 与少量配置 API 查询 HBase，Extension 负责 Projection / Filter 下推、并行 Scan、缓存命中判断与数据合并，Gateway 负责远端数据访问。

**当前状态：设计阶段（V1.0 技术方案已完成，代码尚未开始）。**

## 核心特性

- **SQL 优先**：`SELECT ... FROM hbase_scan(...)` 即可查询 HBase，支持 Projection Pushdown、Filter Pushdown 与并行 Scan。
- **统一 Provider 协议**：Extension 只依赖 `RemoteScanProvider`，不绑定任何一种 HBase 接入方式。
- **多 Gateway 适配**：Native Java Adapter 为默认实现，REST / Thrift / Phoenix 为可插拔实现。
- **列式 Scan Cache**：以 Segment + Coverage Index 管理本地 Parquet 缓存，支持 Full Hit / Partial Hit / Cache Miss。
- **增量补列**：部分命中时按 RowKey 向 Gateway 补齐缺失列，并以 Copy-on-Write 合并。
- **流式传输**：Protobuf + Arrow IPC，禁止全量结果无界加载内存。

## 架构分层

| 层次        | 技术                       | 职责                       |
| --------- | ------------------------ | ------------------------ |
| SQL 层     | DuckDB C++ Extension API | SQL 接入、Scan 执行           |
| 执行层       | DuckDB Table Function    | Projection、Filter、并行执行   |
| Cache 层   | C++ + Parquet + Metadata | Scan 缓存、列复用、补列           |
| 协议层       | Protobuf + Arrow IPC     | 统一远程数据协议                 |
| Gateway 层 | Java / HTTP / gRPC       | HBase 访问适配               |
| 数据源层      | HBase 2.x                | 实际数据存储                   |
| 构建层       | CMake + Maven            | Extension 与 Gateway 构建    |

关键边界：

- Extension 只依赖统一的 `RemoteScanProvider`。
- Gateway SPI 定义远端访问能力，不同 Adapter 实现该 SPI。
- Cache Engine 属于 Extension 核心能力，与具体 Gateway 无关。
- 第一阶段以只读查询为主，不实现 Catalog。

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [技术方案 V1.0](tech_v0.1.md) | 设计基线：架构、模块、协议、API、部署、分包、里程碑 |
| [架构总览](docs/architecture.md) | 分层、依赖方向、模块边界、端到端链路 |
| [Extension 设计](docs/extension-design.md) | Table Function、Scan 计划与执行、下推、并发 |
| [Gateway 设计](docs/gateway-design.md) | Provider 抽象、Gateway SPI、Adapter、装配 |
| [协议定义](docs/protocol.md) | RPC、消息、错误模型、能力协商、版本兼容 |
| [缓存设计](docs/cache-design.md) | Segment、Coverage Index、命中判定、补列、一致性 |
| [用户指南](docs/user-guide.md) | 安装、配置、SQL/Python 示例、缓存管理、排查 |
| [部署指南](docs/deployment-guide.md) | 拓扑、配置、安全、超时重试、监控、故障处置 |
| [测试策略](docs/testing.md) | 测试分层、用例清单、性能基线 |
| [路线图](docs/roadmap.md) | M1-M5 里程碑、任务清单与退出标准 |
| [编码规范](docs/coding-standards.md) | 命名空间、依赖规则、错误模型、提交规范 |
| [AI 编码指南](AGENTS.md) | 阅读顺序、硬约束、工作流、文档同步矩阵 |
| [贡献指南](CONTRIBUTING.md) | 分支、提交、PR、评审、文档贡献 |

## 里程碑

| 阶段 | 核心交付                       | 验收标准                         |
| -- | -------------------------- | ---------------------------- |
| M1 | Extension + Native Gateway | 能执行真实 HBase Scan             |
| M2 | Provider 抽象 + gRPC/Arrow   | Extension 不依赖 Native Adapter |
| M3 | Full Cache Hit             | 重复查询不访问 HBase                |
| M4 | Partial Cache Hit          | 增加列可以按 RowKey 补齐             |
| M5 | 多 Adapter + 生产优化           | 支持多种接入方式、并发与失效               |

详细任务与退出标准见 [路线图](docs/roadmap.md)。

## 仓库结构

```text
duckdb-hbase/
├── AGENTS.md           # AI 编码指南（入口）
├── CLAUDE.md           # 指向 AGENTS.md 的兼容入口
├── CONTRIBUTING.md     # 协作流程与 PR 规范
├── CHANGELOG.md        # 变更记录
├── tech_v0.1.md        # 设计基线
├── docs/               # 工程文档族
├── extension/          # C++ Extension（CMake）
├── gateway/            # Java Gateway（Maven 多模块）
├── protocol/proto/     # Protobuf 协议定义
├── scripts/            # 构建、打包、集成测试脚本
└── examples/           # SQL / Python / 配置示例
```

## 快速开始

M1 进行中：Extension 骨架已可构建并通过 SQLLogicTest；Gateway 骨架（Maven + gRPC + Native Adapter）已就位，Scan 的 Arrow IPC 编码与 Extension 侧 Provider 尚未接入。

```bash
git clone --recurse-submodules https://github.com/lipengming/duckdb-hbase.git
cd duckdb-hbase

# 构建（首次约 15-30 分钟；需要 cmake / ninja / python3 / C++17 编译器）
GEN=ninja EXT_FLAGS="-DCMAKE_POLICY_VERSION_MINIMUM=3.5" make release

# 运行本仓库的 SQLLogicTest
./build/release/test/unittest "*hbase*"

# 手工体验（本地未签名扩展必须加 -unsigned）
./build/release/duckdb -unsigned -c "LOAD 'build/release/extension/hbase/hbase.duckdb_extension'; SELECT hbase_version();"

# Gateway（需要 JDK 11 + Maven）
mvn -q -f gateway/pom.xml -DskipTests package
```

HBase 测试环境准备见 [环境准备清单](docs/environment.md)，实现进展见 [路线图](docs/roadmap.md)。

## 参与贡献

请先阅读 [AGENTS.md](AGENTS.md)（架构硬约束与阅读顺序）与 [CONTRIBUTING.md](CONTRIBUTING.md)（分支、提交、PR 规范）。提交信息遵循 Conventional Commits。

## 许可

许可证待定（License TBD）。在确定许可证之前，本仓库内容默认保留所有权利。
