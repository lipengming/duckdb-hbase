"""错误路径与边界用例。

目标不是「能跑通」，而是失败时可诊断：未注册连接、非法列描述、非法版本参数、
空范围、Gateway 不可达都应给出明确错误（或按方案定义返回空结果）。
"""

from __future__ import annotations

import pytest

from duckdb_hbase_it.config import CONFIG
from duckdb_hbase_it.duckdb_session import table_scan_sql, sql_literal

import it_helpers as helpers

pytestmark = pytest.mark.extension


def test_unregistered_connection(session):
    """IT-EXT-12：未注册连接名 -> Binder 错误提示先 hbase_connect。"""
    sql = (
        "SELECT * FROM hbase_scan("
        f"{sql_literal('missing_conn')}, {sql_literal(CONFIG.namespace)}, "
        f"{sql_literal(CONFIG.table)})"
    )
    with pytest.raises(Exception) as info:
        session.execute(sql)
    assert "未注册" in str(info.value)


def test_invalid_column_spec(connection):
    """IT-EXT-12b：列描述必须是 family:qualifier。"""
    sql = (
        "SELECT * FROM hbase_scan("
        f"{sql_literal(CONFIG.connection)}, {sql_literal(CONFIG.namespace)}, "
        f"{sql_literal(CONFIG.table)}, columns := ['bad'])"
    )
    with pytest.raises(Exception) as info:
        connection.execute(sql)
    assert "非法" in str(info.value)


def test_invalid_max_versions(connection):
    """IT-EXT-12c：max_versions 必须 >= 1。"""
    columns = ["party:payer_id"]
    sql = (
        "SELECT * FROM hbase_scan("
        f"{sql_literal(CONFIG.connection)}, {sql_literal(CONFIG.namespace)}, "
        f"{sql_literal(CONFIG.table)}, max_versions := 0, "
        f"columns := [{', '.join(sql_literal(item) for item in columns)}])"
    )
    with pytest.raises(Exception) as info:
        connection.execute(sql)
    assert "max_versions" in str(info.value)


def test_columns_required(connection):
    """IT-EXT-12e：M1 不支持省略 columns（必须显式报错，而不是静默返回 NULL）。"""
    sql = (
        "SELECT * FROM hbase_scan("
        f"{sql_literal(CONFIG.connection)}, {sql_literal(CONFIG.namespace)}, "
        f"{sql_literal(CONFIG.table)})"
    )
    with pytest.raises(Exception) as info:
        connection.execute(sql)
    assert "columns" in str(info.value)


def test_multi_version_expands(connection):
    """IT-EXT-12f：max_versions > 1 时按时间戳展开，并返回 __timestamp 列。"""
    sql = (
        "SELECT \"cf2:score\", \"__timestamp\" FROM hbase_scan("
        f"{sql_literal(CONFIG.connection)}, {sql_literal('duckdb_test')}, "
        f"{sql_literal('customer_profile')}, start_row := 'r011', stop_row := 'r012', "
        "columns := ['cf2:score'], max_versions := 3)"
    )
    rows = connection.fetchall(sql)
    assert len(rows) >= 2, f"r011 应有多个版本，实际 {rows}"
    values = {row[0] for row in rows}
    assert {"11", "99"} <= values, values
    timestamps = [row[1] for row in rows]
    assert len(set(timestamps)) == len(timestamps), "同一 RowKey 的时间戳不应重复"


def test_cache_true_rejected(connection):
    """IT-EXT-12g：缓存未实现时显式 cache := true 必须报错，不能静默忽略。"""
    sql = (
        "SELECT * FROM hbase_scan("
        f"{sql_literal(CONFIG.connection)}, {sql_literal(CONFIG.namespace)}, "
        f"{sql_literal(CONFIG.table)}, cache := true, columns := ['party:payer_id'])"
    )
    with pytest.raises(Exception) as info:
        connection.execute(sql)
    assert "缓存" in str(info.value)


def test_gateway_unreachable(session):
    """IT-EXT-12d：Gateway 端口不可达时建连失败并给出端点信息。"""
    with pytest.raises(Exception) as info:
        session.scalar(
            "SELECT hbase_connect('bad_gateway', 'grpc://127.0.0.1:1')"
        )
    message = str(info.value)
    assert "Gateway" in message or "不可用" in message


def test_empty_range_returns_zero_rows(scan_ready, manifest):
    """IT-EXT-12e：start_row > stop_row 的空范围返回 0 行且不报错。"""
    start, stop = helpers.hot_range(manifest)
    sql = "SELECT count(*) FROM " + table_scan_sql(
        connection=CONFIG.connection,
        namespace=CONFIG.namespace,
        table=CONFIG.table,
        columns=["party:payer_id"],
        start_row=stop,
        stop_row=start,
    )
    assert scan_ready.scalar(sql) == 0
