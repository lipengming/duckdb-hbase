# Changelog

本文件记录本仓库的显著变更。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [Unreleased]

### Added

- 仓库工程骨架：`docs/` 文档体系、`extension/` `gateway/` `protocol/` `scripts/` `examples/` 目录说明。
- AI 编码指南 `AGENTS.md`（含架构硬约束、阅读顺序、文档同步矩阵）与 `CLAUDE.md` 入口。
- 协作与规范文档：`CONTRIBUTING.md`、`docs/coding-standards.md`。
- 设计文档族：`architecture.md`、`extension-design.md`、`gateway-design.md`、`protocol.md`、`cache-design.md`、`user-guide.md`、`deployment-guide.md`、`testing.md`、`roadmap.md`。
- 协议草案 `protocol/proto/hbase_data_service.proto`。
- 工程配置：`.editorconfig`、`.clang-format`、`.gitattributes`、`.gitignore`、Issue/PR 模板。

## [0.1.0] - 2026-11-01

### Added

- 技术方案 V1.0（`tech_v0.1.md`）：架构分层、Extension 模块设计、Gateway 抽象与多接入适配、缓存引擎、用户 API、安装部署、分包设计、里程碑规划。
