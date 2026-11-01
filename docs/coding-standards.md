# 编码规范

> 提炼自 [技术方案 V1.0](../tech_v0.1.md) §6，并结合本仓库工程约定扩展。若与方案冲突，以方案为准。

本规范适用于 `extension/`（C++）、`gateway/`（Java）、`protocol/`（Protobuf）以及 `scripts/`、`examples/` 下的所有代码与配置。

## 1. 通用原则

1. **先设计后编码**：任何跨越模块边界的改动，先在 `docs/` 对应设计文档中写清接口与数据流，再落代码。
2. **显式优于隐式**：不做跨层依赖、不依赖全局可变状态、不依赖调用顺序的隐含约定。
3. **失败要可诊断**：错误信息必须携带上下文（表名、RowKey 范围、列族、Gateway 端点、Segment ID），且不得泄露凭据。
4. **热点路径零浪费**：Scan 热路径禁止逐行日志、禁止不必要的拷贝、禁止阻塞式 IO。
5. **文档与代码同源**：接口、配置项、错误码变更必须同 PR 更新 `docs/`。

## 2. 目录与命名空间

### 2.1 C++ 命名空间

```cpp
namespace duckdb {
namespace hbase {
namespace sql {}        // Table Function、SQL 函数注册
namespace scan {}       // 计划、执行、并行、谓词与投影
namespace cache {}      // 命中判定、覆盖索引、合并、失效、淘汰
namespace storage {}    // Parquet、内存、元数据、Segment Manifest
namespace provider {}   // RemoteScanProvider 及协议客户端实现
namespace catalog {}    // Schema 获取、列映射、类型映射
namespace common {}     // 配置、错误、日志、RowKey、类型工具
}
}
```

- 头文件放在 `extension/include/<domain>/`，实现放在 `extension/src/<domain>/`。
- 公开头文件只暴露接口，不暴露实现细节与第三方类型（PIMPL 或前向声明）。
- C++ 头文件统一使用 `#pragma once`。

### 2.2 Java 包名

- 根包：`com.duckdb.hbase`
- `com.duckdb.hbase.protocol`：Protobuf 生成类与 DTO 转换
- `com.duckdb.hbase.spi`：`HBaseAccessAdapter` 等 SPI 接口
- `com.duckdb.hbase.core`：服务实现、Factory、流式传输、安全
- `com.duckdb.hbase.adapter.native|rest|thrift|phoenix`：各接入实现
- `com.duckdb.hbase.server`：gRPC 入口与装配

## 3. 依赖规则（硬约束）

| 规则 | 说明 |
| --- | --- |
| `scan` 不得 include `grpc_provider.hpp` | `scan` 只依赖 `remote_scan_provider.hpp`，否则 Extension 与具体 Gateway 耦合 |
| `cache` 不得依赖任何 Gateway 类型 | 缓存策略、命中判断、合并全部由 Extension 统一管理 |
| `adapter-*` 只依赖 `gateway-api` | 不得依赖 `gateway-core` 或 `gateway-server` |
| `gateway-server` 不得直接引用具体 Adapter 实现 | 只能通过 Factory 装配 `HBaseAccessAdapter` |
| `extension` 不得直接依赖 HBase 客户端库 | HBase 访问只发生在 Gateway |
| 新增第三方依赖必须评审 | 需说明许可证、体积、可选替代，并在 PR 中记录 |

依赖方向（两侧一致，不得反向）：

```text
extension(scan/cache) ──> provider(RemoteScanProvider) ──> gateway(gRPC/HTTP) ──> HBase
gateway-server ──> gateway-core ──> gateway-api <── adapter-*
```

## 4. C++ 规范

- 标准：C++17（若 DuckDB 基线要求更低，以 DuckDB 扩展构建要求为准）。
- 格式化：`clang-format`（仓库根 `.clang-format`，基样式 LLVM，4 空格缩进），提交前必须执行。
- 命名：类型 `PascalCase`；函数与局部变量 `snake_case`；成员变量 `snake_case_`；常量 `kPascalCase` 或全大写（同一文件内保持一致）。
- 所有权：一律 RAII；禁止裸 `new`/`delete`；跨线程传递用 `std::shared_ptr`，独占用 `std::unique_ptr`。
- 错误处理：
  - 用户输入错误 → `duckdb::InvalidInputException`
  - 远端/IO 错误 → `duckdb::IOException` 或项目自定义异常
  - 禁止 `exit`/`abort`/吞异常；捕获后必须补充上下文再抛
- 并发：
  - 每个类的线程安全性必须在头文件注释中写明（`thread-safe` / `not thread-safe` / `guarded by mutex`）
  - Scan 并行度由 DuckDB 调度，LocalState 不得跨线程共享
  - 缓存写入使用 Copy-on-Write + 临时文件 `fsync` + `rename` 原子提交
- 内存与流式：禁止全量结果集驻留内存；Arrow RecordBatch 处理完即释放。
- 日志：统一 `common/logger`，禁止 `printf`/`std::cout`；按行数据不得打印。
- 单元测试：`extension/test/unit/`，使用 DuckDB 测试框架，新增逻辑必须覆盖正常路径与边界。

## 5. Java 规范

- 版本基线：Java 11（HBase 2.x 支持的 LTS 范围内；最终以集群版本为准，待定）。
- 构建：Maven 多模块，版本与依赖统一在父 `pom.xml` 的 `dependencyManagement` 管理。
- 命名：包名全小写；类 `PascalCase`；方法/变量 `camelCase`；常量 `UPPER_SNAKE_CASE`。
- DTO：不可变（`final` 字段 + 构造器），禁止在协议 DTO 上承载业务行为。
- 依赖注入：构造器注入，禁止字段注入；便于单测替换。
- 异常：SPI 实现只抛 `HBaseAccessException` 体系内的异常，底层 HBase/HTTP 异常必须转换并保留 cause。
- 资源：`try-with-resources` 管理 `Connection`、`Table`、`Scanner`、`Stream`；禁止跨请求复用未加保护的 HBase 连接对象（以客户端线程安全说明为准）。
- 日志：SLF4J，禁止 `System.out`；必须记录 request id 与耗时。
- 并发：共享状态用 `java.util.concurrent` 原语；禁止在请求线程上做无界阻塞队列操作。
- 测试：JUnit 5；SPI 实现用 Mock 隔离真实 HBase。

## 6. Protobuf 与协议

- 文件位置：`protocol/proto/*.proto`，包名 `duckdb.hbase.v1`。
- 兼容规则：
  - 字段**只增不删**；删除字段必须 `reserved` 编号与名称
  - 字段编号一旦发布不得复用，不得修改既有字段类型与语义
  - 新增字段必须有注释与默认值语义说明
- 每次协议变更：更新 `docs/protocol.md`、在 PR 描述中写明兼容性影响。
- 生成代码不提交；由构建流程生成，`protocol/gen/` 之类目录加入 `.gitignore`。

## 7. SQL 接口约定

- 函数统一 `hbase_` 前缀，`snake_case`；缓存管理函数使用 `hbase_cache_*` 前缀（以方案 §4 为准）。
- 命名参数使用 `key = value` 形式，参数名与文档、示例保持一致。
- 新增命名参数必须同步更新 `docs/user-guide.md` 的参数表。

## 8. 错误模型

| 层次 | 错误类型 | 可重试 |
| --- | --- | --- |
| Extension 参数/类型 | `InvalidInputException` | 否 |
| 远端不可达 / 超时 | `IOException`（带 endpoint、超时值） | 是（有上限） |
| 协议不兼容 / 能力不足 | 自定义协议异常 | 否 |
| 缓存损坏 | 缓存异常 → 作废 Segment 并回退远端 | 是（自动一次） |
| HBase 鉴权失败 | Gateway 侧 `HBaseAccessException` | 否 |

错误信息统一格式：`<操作> failed: <原因> [context: key=value, ...]`，禁止拼接密码、Token、Kerberos 票据。

## 9. 提交与评审

- 提交信息遵循 Conventional Commits：`<type>(<scope>): <subject>`
  - type：`feat` `fix` `docs` `refactor` `test` `build` `chore` `perf` `ci`
  - scope：`extension` `scan` `cache` `gateway` `proto` `docs` `ci`
  - 标题 ≤ 72 字符，使用祈使句
- 一个提交只做一件事；格式化与逻辑改动分开提交。
- 提交前自检：格式化、编译、单测、文档同步、无密钥与大文件。

## 10. 文档同步矩阵

| 改动内容 | 必须同步的文档 |
| --- | --- |
| Extension 模块/接口 | `docs/extension-design.md`、`docs/architecture.md` |
| Provider/SPI/协议 | `docs/gateway-design.md`、`docs/protocol.md`、`protocol/proto/*.proto` |
| 缓存策略/存储布局 | `docs/cache-design.md` |
| SQL 函数与参数 | `docs/user-guide.md` |
| 部署拓扑/配置/安全 | `docs/deployment-guide.md` |
| 里程碑范围 | `docs/roadmap.md` |
| 测试策略 | `docs/testing.md` |
| 对设计基线的偏离 | `tech_v0.1.md`（新增章节或标注废弃段落）+ `CHANGELOG.md` |
