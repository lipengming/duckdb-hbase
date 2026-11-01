"""DuckDB-HBase 集成测试包。

职责边界：

- 生成确定性的「客户交易宽表」mock 数据（中文内容，单客户 10 万/50 万/100 万三档）；
- 在 HBase 上建表并批量装载数据（Java BufferedMutator，避免逐行 shell put）；
- 用 pytest 定义并执行数据面、Extension（DuckDB + hbase.duckdb_extension）用例。

设计说明见 ``integration/README.md`` 与 ``docs/testing.md`` §5。
"""

__version__ = "0.1.0"
