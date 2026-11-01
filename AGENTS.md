# AGENTS.md — AI 编码指南

本文件是本仓库对 AI 编码代理（以及新加入的人类工程师）的统一入口。**开始任何任务前先读完本文件**，再按第 3 节的阅读顺序加载上下文。

## 1. 项目一句话

DuckDB-HBase 是一个 DuckDB C++ Extension：通过统一 `RemoteScanProvider` 协议访问 HBase，具备本地列式 Scan Cache（Segment + Coverage Index），支持 Native Java / REST / Thrift / Phoenix 多种 Gateway 接入方式。

## 2. 当前阶段（重要）

- **设计阶段已完成**：`tech_v0.1.md` 是 V1.0 技术方案的唯一基线，共 13 章。
- **M1 进行中**：`extension/` 已有可编译骨架（`hbase_version` / `hbase_connect` / `hbase_disconnect` / `hbase_scan` 参数解析），能通过 `make release` 构建并跑 SQLLogicTest；Extension 侧的 Provider 尚未接入。
- **Gateway 骨架已就位**：`gateway/` 有 Maven 多模块、Adapter SPI、Native Adapter 与 gRPC 服务；Health / GetCapabilities / GetSchema / Cancel 可用，Scan 与 BatchGet 因 Arrow IPC 编码未完成返回 `UNIMPLEMENTED`。
- **尚无缓存与真实 HBase 端到端链路**：`hbase_scan` 在 Bind 阶段显式报错（Provider 未接入），不要把它当成可用功能。
- 任何实现工作都对应 `docs/roadmap.md` 中的 M1-M5 里程碑；没有对应里程碑的“顺手重构”不做。

## 3. 阅读顺序（按任务类型）

| 任务 | 必读 | 选读 |
| --- | --- | --- |
| 任意任务 | 本文件、`docs/architecture.md` | `README.md` |
| 写 Extension / 扫描逻辑 | `docs/extension-design.md`、`docs/coding-standards.md` | `tech_v0.1.md` §2、§7 |
| 写缓存逻辑 | `docs/cache-design.md`、`docs/extension-design.md` | `tech_v0.1.md` §2.6-2.7、§7.3-7.6 |
| 写 Gateway / Adapter | `docs/gateway-design.md`、`docs/protocol.md` | `tech_v0.1.md` §3、§6.2 |
| 改协议 | `docs/protocol.md`、`protocol/proto/*.proto` | `tech_v0.1.md` §3.5 |
| 改 SQL / 参数 | `docs/user-guide.md`、`docs/extension-design.md` | `tech_v0.1.md` §4 |
| 部署 / 安全 | `docs/deployment-guide.md` | `tech_v0.1.md` §8 |
| 测试 / 验收 | `docs/testing.md`、`docs/roadmap.md` | `tech_v0.1.md` §9-§10 |

## 4. 仓库地图

```text
duckdb-hbase/
├── AGENTS.md                 # 本文件：AI 编码入口
├── CLAUDE.md                 # 指向本文件的兼容入口
├── README.md                 # 项目介绍与文档索引
├── CONTRIBUTING.md           # 协作流程、分支与 PR 规范
├── CHANGELOG.md              # 版本变更记录
├── tech_v0.1.md              # 设计基线（V1.0 技术方案，权威）
├── Makefile                  # 封装 extension-ci-tools 的构建/测试目标
├── extension_config.cmake    # 告诉 DuckDB 构建哪个扩展（SOURCE_DIR=extension/）
├── vcpkg.json                # C++ 依赖清单（当前为空，接 gRPC 时再补）
├── docs/                     # 工程文档（由设计基线派生）
│   ├── architecture.md       # 分层、依赖方向、模块边界
│   ├── extension-design.md   # C++ Extension 设计
│   ├── gateway-design.md     # Gateway 抽象与 Adapter
│   ├── protocol.md           # 远程扫描协议与错误模型
│   ├── cache-design.md       # 缓存引擎与一致性
│   ├── user-guide.md         # 用户使用指南
│   ├── deployment-guide.md   # 部署、安全、运维
│   ├── testing.md            # 测试策略
│   ├── roadmap.md            # M1-M5 里程碑
│   └── coding-standards.md   # 编码规范（硬约束）
├── extension/                # C++ Extension（独立构建单元）
│   ├── CMakeLists.txt        # 扩展构建入口
│   ├── src/                  # 源码：include/ + common/ + sql/
│   └── test/sql/             # SQLLogicTest
├── gateway/                  # Java Gateway（Maven 多模块：api/core/adapter-native/server）
├── protocol/proto/           # Protobuf 定义
├── scripts/                  # 环境自检、HBase 夹具、构建脚本
├── examples/                 # SQL / Python / 配置示例
├── duckdb/                   # submodule：DuckDB v2.0.0 源码（构建依赖，勿改）
└── extension-ci-tools/       # submodule：官方扩展构建/测试脚本（勿改）
```

## 5. 不可违反的架构约束

1. Extension 只依赖 `RemoteScanProvider`，**不得**直接依赖任何一种 HBase Gateway 或 HBase 客户端。
2. Cache Engine 属于 Extension 核心能力，**不得**下沉到 Gateway。
3. `scan` 模块只 include `remote_scan_provider.hpp`，不得 include `grpc_provider.hpp` 等具体实现。
4. `adapter-*` 只依赖 `gateway-api`；`gateway-server` 不直接引用具体 Adapter，只通过 Factory 装配。
5. 所有结果流式传输（Arrow IPC），**禁止**全量结果无界加载内存。
6. 部分补列一律以 RowKey 为关联键，**禁止**用行号/位置假设行对齐。
7. 第一阶段只读、不实现 Catalog：不要"顺手"加写入、DDL 或 Catalog 功能。

违反以上任一条的实现方案，即使能跑通也必须打回。

## 6. 工作流（每个任务都走）

1. **定位**：把需求映射到 `docs/roadmap.md` 的里程碑与 `docs/` 的设计文档章节。
2. **读基线**：打开 `tech_v0.1.md` 对应章节，确认接口、字段、语义以基线为准。
3. **写计划**：多文件改动先列出将改动的文件与验证方式（`todo_write` 或 PR 描述）。
4. **实现**：遵循 `docs/coding-standards.md`；新增接口先写头文件/接口定义，再写实现。
5. **验证**：格式化 → 编译 → 单测 → 集成测试（有环境时）；记录未验证项。
6. **同步文档**：按第 8 节矩阵更新文档，必要时更新 `CHANGELOG.md`。
7. **提交**：Conventional Commits，一个提交一件事。

## 7. 常用命令

```bash
# 查看仓库状态与最近提交
git status && git log --oneline -10

# 全文检索（优先使用工具而非 cat）
rg "hbase_scan" docs/ extension/src/ tech_v0.1.md

# 拉取 DuckDB / extension-ci-tools 子模块（首次 clone 必须）
git submodule update --init --recursive

# 构建 Extension（首次约 20 分钟，之后增量很快）
GEN=ninja EXT_FLAGS="-DCMAKE_POLICY_VERSION_MINIMUM=3.5" make release

# 运行 SQLLogicTest（可用路径过滤，只跑本仓库测试）
./build/release/test/unittest "*hbase*"

# 手工验证：-unsigned 是本地未签名扩展必须的
./build/release/duckdb -unsigned -c "LOAD 'build/release/extension/hbase/hbase.duckdb_extension'; SELECT hbase_version();"

# 环境自检与 HBase 夹具（需要 HBase 2.6.x 已启动）
bash scripts/check-env.sh
bash scripts/hbase-fixture.sh

# Java Gateway：构建 + 单测（需要 JDK 11 / Maven）
mvn -q -f gateway/pom.xml -DskipTests package
mvn -q -f gateway/pom.xml test

# 启动 Gateway（先按 gateway/conf/application.properties 配好 ZooKeeper 地址）
# java -jar gateway/gateway-server/target/gateway-server-0.1.0-SNAPSHOT.jar gateway/conf/application.properties
```

## 8. 文档同步矩阵

| 代码改动 | 必须更新的文档 |
| --- | --- |
| Extension 模块、类、接口 | `docs/extension-design.md`、`docs/architecture.md` |
| Provider / SPI / gRPC 协议 | `docs/gateway-design.md`、`docs/protocol.md`、`protocol/proto/*.proto` |
| 缓存策略、Segment 布局、失效 | `docs/cache-design.md` |
| SQL 函数、参数、示例 | `docs/user-guide.md`、`examples/` |
| 部署、配置、安全 | `docs/deployment-guide.md` |
| 测试范围、验收标准 | `docs/testing.md`、`docs/roadmap.md` |
| 与设计基线不一致的决策 | `tech_v0.1.md`（新增修订章节）+ `CHANGELOG.md` |

## 9. 禁止事项

- 不要修改 `tech_v0.1.md` 的既有结论来"让代码看起来合理"；偏离基线必须先写 ADR 式说明并在 PR 中确认。
- 不要引入未经评审的新依赖（尤其 HBase 客户端、Arrow/Parquet 之外的 C++ 库、HTTP/gRPC 框架）。
- 不要提交：密钥、Token、Kerberos keytab、真实集群地址、缓存数据、构建产物、IDE 配置（见 `.gitignore`）。
- 不要用 `git push --force` 到 `main`，不要改写已推送历史。
- 不要写"未来会实现"的空实现并标记为完成；未完成的功能在文档中明确标 `待定（TBD）` 并链接到 roadmap 任务。

## 10. 完成定义（Definition of Done）

一个任务可交付，需要同时满足：

- [ ] 行为符合 `tech_v0.1.md` 对应章节，偏差已记录
- [ ] 代码通过格式化与编译，无新增警告（-Wall 级别）
- [ ] 新增/修改逻辑有对应单测；缓存与协议改动有集成测试或明确说明未测原因
- [ ] 错误路径返回可诊断错误，不含敏感信息
- [ ] 相关文档与 `CHANGELOG.md` 已更新
- [ ] 提交信息符合规范，PR 描述包含验证方式与风险点

## 11. 不确定时怎么办

- 设计取舍不明确 → 优先选择**更少耦合、更易回滚**的方案，并在 PR 中列出被放弃的选项。
- 基线未覆盖的问题 → 在对应 `docs/*.md` 中新增"待定（TBD）"小节，写清需要的输入，不要自行拍板。
- 影响跨模块接口（Provider 协议、缓存格式、SQL 签名）→ 先提 issue/PR 讨论，不改动已知结论。
