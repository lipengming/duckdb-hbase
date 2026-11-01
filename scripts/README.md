# scripts — 构建与运维脚本

本目录存放可复用的构建、打包、集成测试脚本。约定：`bash` + `set -euo pipefail`，参数用环境变量或显式 flag，不硬编码路径与凭据。

## 已实现脚本

| 脚本 | 作用 | 文档 |
| --- | --- | --- |
| `install-dev-env.sh` | 安装本机开发环境四件套（JDK 11 / Maven / HBase 2.6.6 / DuckDB CLI 2.0.0），幂等 | [开发环境说明](../docs/environment.md) |
| `check-env.sh` | 工具链、JDK/Maven、DuckDB、HBase 连通性、磁盘内存自检 | [开发环境说明](../docs/environment.md) |
| `hbase-fixture.sh` | 建 `duckdb_test` 命名空间、测试表与夹具数据 | [测试策略](../docs/testing.md) |

## 计划脚本

| 脚本 | 作用 | 状态 |
| --- | --- | --- |
| `build-extension.sh` | 构建 C++ Extension 产物 | 待实现（M1） |
| `build-gateway.sh` | Maven 构建 Gateway 与 Adapter | 待实现（M2） |
| `package.sh` | 打包 Extension 与 Gateway 发行物 | 待实现（M3） |
| `integration-test.sh` | 启动依赖并跑端到端集成测试 | 待实现（M1） |

## 约定

- 脚本必须幂等，可重复执行；失败时输出可定位的错误上下文。
- 不在脚本中读取真实集群凭据；通过环境变量注入，且不得回显。
- 新增脚本需在 [`../docs/deployment-guide.md`](../docs/deployment-guide.md) 与 [`../docs/testing.md`](../docs/testing.md) 中登记用途。
- 例外：`install-dev-env.sh` 有意不使用 `set -e`，改为收集全部失败后统一汇总（一次跑完暴露所有网络/权限问题），详见脚本头部注释。
