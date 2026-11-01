# extension — C++ DuckDB Extension

本目录承载 C++ 侧实现：SQL 接入、Scan 执行、缓存引擎与远端 Provider 客户端。构建系统为 CMake，产物为 `*.duckdb_extension`。

## 职责边界

- 只依赖统一协议 `RemoteScanProvider`，不依赖具体 Gateway 实现。
- Cache Engine（Segment、Coverage Index、Parquet 存储）属于本目录，不下沉到 Gateway。
- 结果以 Arrow 流式处理，禁止全量加载内存。

## 计划结构

```text
extension/
├── include/          # 对外接口头文件（scan/ cache/ storage/ provider/ common/）
├── src/              # 实现（extension/ sql/ scan/ cache/ storage/ provider/ catalog/ common/）
├── test/             # 单元测试、SQL 测试、集成测试
└── CMakeLists.txt
```

## 当前状态

仅目录占位，尚无代码。M1 目标见 [`../docs/roadmap.md`](../docs/roadmap.md)，设计见 [`../docs/extension-design.md`](../docs/extension-design.md)。

## 编码约定

命名空间 `duckdb::hbase::{sql,scan,cache,storage,provider,catalog,common}`；`scan` 不得 include 具体 Provider 实现头文件。详见 [`../docs/coding-standards.md`](../docs/coding-standards.md)。
