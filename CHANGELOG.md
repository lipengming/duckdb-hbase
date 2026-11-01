# Changelog

本文件记录本仓库的显著变更。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [Unreleased]

### Added

- **M1 Extension 骨架**：`hbase_version()`、`hbase_connect()` / `hbase_disconnect()`（端点解析、会话级连接表）、`hbase_scan()` 参数解析与校验（远端 Provider 未接入前显式报错）。
- **构建体系**：`CMakeLists.txt`、`Makefile`、`extension_config.cmake`、`vcpkg.json`；`duckdb`（v2.0.0）与 `extension-ci-tools` submodule。
- **测试**：`test/sql/hbase_basic.test`（注册、幂等覆盖、非法端点、未注册连接、非法列描述等 10 个断言）。
- **环境工具**：`scripts/check-env.sh`（工具链/HBase 自检）、`scripts/hbase-fixture.sh`（建表与夹具数据）、`docs/environment.md`（环境准备清单）。

- 仓库工程骨架：`docs/` 文档体系、`extension/` `gateway/` `protocol/` `scripts/` `examples/` 目录说明。
- AI 编码指南 `AGENTS.md`（含架构硬约束、阅读顺序、文档同步矩阵）与 `CLAUDE.md` 入口。
- 协作与规范文档：`CONTRIBUTING.md`、`docs/coding-standards.md`。
- 设计文档族：`architecture.md`、`extension-design.md`、`gateway-design.md`、`protocol.md`、`cache-design.md`、`user-guide.md`、`deployment-guide.md`、`testing.md`、`roadmap.md`。
- 协议草案 `protocol/proto/hbase_data_service.proto`。
- 工程配置：`.editorconfig`、`.clang-format`、`.gitattributes`、`.gitignore`、Issue/PR 模板。

## [0.1.0] - 2026-11-01

### Added

- 技术方案 V1.0（`tech_v0.1.md`）：架构分层、Extension 模块设计、Gateway 抽象与多接入适配、缓存引擎、用户 API、安装部署、分包设计、里程碑规划。
