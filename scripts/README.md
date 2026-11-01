# scripts — 构建与运维脚本

本目录存放可复用的构建、打包、集成测试脚本。约定：`bash` + `set -euo pipefail`，参数用环境变量或显式 flag，不硬编码路径与凭据。

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
