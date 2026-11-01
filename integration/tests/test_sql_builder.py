"""SQL 拼装工具的单测（不需要 DuckDB / HBase）。

``hbase_scan(...)`` 的调用由测试侧拼装，必须保证字面量转义与列名引用正确，
否则会把 RowKey/列名中的特殊字符带进 SQL。
"""

from __future__ import annotations

from duckdb_hbase_it.duckdb_session import quote_identifier, sql_literal, table_scan_sql


def test_sql_literal_escapes_single_quotes():
    assert sql_literal("plain") == "'plain'"
    assert sql_literal("O'Brien") == "'O''Brien'"
    assert sql_literal("') DROP TABLE x; --") == "''') DROP TABLE x; --'"


def test_quote_identifier_keeps_colon_and_escapes_quotes():
    assert quote_identifier("party:payer_id") == '"party:payer_id"'
    assert quote_identifier('we"ird') == '"we""ird"'


def test_table_scan_sql_contains_range_and_columns():
    sql = table_scan_sql(
        connection="it",
        namespace="duckdb_test",
        table="customer_transaction",
        columns=["party:payer_id", "txn:amount"],
        start_row="0dU000001#",
        stop_row="0dU000001$",
        cache=False,
    )
    assert sql.startswith("hbase_scan(")
    assert "start_row := '0dU000001#'" in sql
    assert "stop_row := '0dU000001$'" in sql
    assert "columns := ['party:payer_id', 'txn:amount']" in sql
    assert "cache := false" in sql


def test_table_scan_sql_omits_optional_parameters():
    sql = table_scan_sql(
        connection="it",
        namespace="duckdb_test",
        table="customer_transaction",
        columns=["party:payer_id"],
    )
    assert "start_row" not in sql
    assert "stop_row" not in sql
    assert "cache" not in sql
