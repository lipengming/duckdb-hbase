# 贡献指南

本仓库采用 Monorepo：C++ Extension 与 Java Gateway 共用同一仓库，但各自使用独立构建系统（CMake / Maven）。

## 1. 开始之前

1. 阅读 [`AGENTS.md`](AGENTS.md)（AI 与人类通用入口）与 [`docs/architecture.md`](docs/architecture.md)。
2. 阅读与你要改动模块对应的设计文档（见 AGENTS.md 第 3 节阅读顺序）。
3. 确认任务对应 [`docs/roadmap.md`](docs/roadmap.md) 中的里程碑条目；不在里程碑内的改动先开 Issue 讨论。

## 2. 环境要求

| 组件 | 版本 | 用途 |
| --- | --- | --- |
| CMake | ≥ 3.5 | Extension 构建（最终以 DuckDB 扩展要求为准，待定） |
| C++ 编译器 | 支持 C++17 | Extension |
| JDK | 11（待定，以 HBase 2.x 集群为准） | Gateway |
| Maven | ≥ 3.8 | Gateway 多模块构建 |
| DuckDB | 1.x | 运行与测试 Extension |
| HBase | 2.x | 集成测试 |
| protoc | 与 `protocol/proto` 匹配 | 生成 Java/C++ 协议代码 |

## 3. 分支与提交

- 主分支：`main`，始终保持可构建、文档自洽。
- 功能分支：`feat/<scope>-<short-desc>`，修复分支：`fix/<scope>-<short-desc>`，文档分支：`docs/<short-desc>`。
- 提交信息遵循 [Conventional Commits](https://www.conventionalcommits.org/)：

```text
<type>(<scope>): <subject>

<body 说明动机与影响>

<footer 关联 issue>
```

`type` 取值：`feat` `fix` `docs` `refactor` `test` `build` `chore` `perf` `ci`
`scope` 取值：`extension` `scan` `cache` `gateway` `proto` `docs` `ci`

示例：

```text
feat(scan): implement projection pushdown for hbase_scan
docs(cache): clarify coverage index gap calculation
```

规则：

- 一个提交只做一件事；格式化与逻辑变更分开提交。
- 标题使用祈使句、≤ 72 字符，不以句号结尾。
- 禁止对 `main` 强制推送，禁止改写已推送历史。

## 4. 代码风格

| 语言 / 文件 | 工具 | 要求 |
| --- | --- | --- |
| C++ | `clang-format`（仓库根 `.clang-format`） | 提交前必须格式化 |
| Java | Maven 默认编译 + 团队约定（见 `docs/coding-standards.md`） | 无未使用 import、无 `System.out` |
| 通用 | `.editorconfig` | UTF-8、LF、行尾去空格、文件末尾换行 |
| Markdown | 人工检查 | 标题层级连续、表格对齐、链接可点击 |

完整规范见 [`docs/coding-standards.md`](docs/coding-standards.md)。

## 5. Pull Request

PR 描述请包含：

1. **目的**：解决什么问题，对应哪个里程碑 / issue。
2. **改动范围**：涉及模块与关键文件。
3. **设计一致性**：与 `tech_v0.1.md` 哪些章节对应；如有偏离，说明原因与影响。
4. **验证方式**：实际执行的构建、单测、集成测试与外，未执行项说明原因。
5. **风险与回滚**：兼容性、性能、数据正确性风险。

评审通过条件：

- [ ] 符合 AGENTS.md 第 5 节架构约束
- [ ] 通过编译与单测，关键路径有测试
- [ ] 文档同步完成（见 AGENTS.md 第 8 节矩阵）
- [ ] `CHANGELOG.md` 的 `Unreleased` 已更新（用户可见变更）
- [ ] 无密钥、无大文件、无构建产物

## 6. 文档贡献

- `tech_v0.1.md` 是设计基线，**不直接改写结论**；需要修订时在文件末尾追加"修订记录"章节，说明原结论、修订原因与影响范围。
- `docs/*.md` 是工程派生文档，可自由改进表述，但事实必须与基线一致；基线未覆盖的内容标注 `待定（TBD）`。
- 新增文档需在两处登记：`README.md` 文档索引、`AGENTS.md` 第 4 节仓库地图。

## 7. Issue

- Bug：复现步骤、期望 / 实际行为、DuckDB 与 HBase 版本、日志（脱敏）。
- Feature：使用场景、对应里程碑、与现有设计的关系。
- 设计讨论：涉及 Provider 协议、缓存格式、SQL 签名等跨模块接口的变更，先用 Issue 收敛方案再写代码。

## 8. 提交前检查清单

```bash
git status                    # 无意外文件
git diff --check              # 无空白错误
clang-format -n --Werror $(git ls-files '*.cpp' '*.hpp')   # 有 C++ 代码后执行
git log --oneline -5          # 提交信息符合规范
```

确认 `.gitignore` 覆盖了构建产物、缓存目录与本地配置，且没有任何凭据被提交。
