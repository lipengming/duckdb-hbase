"""Extension 端到端用例（DuckDB + hbase.duckdb_extension + Gateway + HBase）。

用例按 ``docs/testing.md`` §4/§5.3 组织，覆盖：投影、过滤、范围、分页、聚合、
数据一致性、NULL/空串语义与连接管理。

当前 M1 阶段 ``hbase_scan`` 数据面未实现，涉及真实扫描的用例通过
``scan_ready`` 夹具自动 skip；实现后无需修改本文件即可执行。
"""

from __future__ import annotations

from collections import Counter
from decimal import Decimal

import pytest

from duckdb_hbase_it import schema
from duckdb_hbase_it.config import CONFIG
from duckdb_hbase_it.duckdb_session import table_scan_sql
from duckdb_hbase_it.generator import Dataset, DatasetConfig

import it_helpers as helpers

pytestmark = pytest.mark.extension

ALL_COLUMNS = list(schema.COLUMN_SPECS)


def q(column: str) -> str:
    return '"' + column + '"'


def dataset_from_manifest(manifest) -> Dataset:
    dataset = manifest["dataset"]
    return Dataset(
        DatasetConfig(
            total_rows=dataset["total_rows"],
            hot_rows=dataset["hot_rows"],
            days=dataset["days"],
            base_date=dataset["base_date"],
            seed=dataset["seed"],
            ordinary_customers=dataset["ordinary_customers"],
            namespace=manifest["table"]["namespace"],
            table=manifest["table"]["name"],
        )
    )


# ---------------------------------------------------------------------------
# 加载 / 连接
# ---------------------------------------------------------------------------


def test_extension_loaded(session):
    """IT-EXT-01：extension 可加载并返回版本。"""
    assert session.version
    assert session.extension_path.is_file()
    assert session.scalar("SELECT hbase_version()") == session.version


def test_connect_and_disconnect(connection, manifest):
    """IT-EXT-02：连接可重复注册；不存在的连接 disconnect 返回 false。"""
    name = "it_tmp"
    sql = (
        "SELECT hbase_connect("
        f"'{name}', '{CONFIG.gateway}')"
    )
    assert connection.scalar(sql) is True
    assert connection.scalar(f"SELECT hbase_disconnect('{name}')") is True
    assert connection.scalar(f"SELECT hbase_disconnect('{name}')") is False


# ---------------------------------------------------------------------------
# 扫描语义
# ---------------------------------------------------------------------------


def test_projection_and_schema(scan_ready, manifest):
    """IT-EXT-03：显式投影只返回请求列，列名与 HBase 列一致。"""
    start, stop = helpers.hot_range(manifest)
    columns = ["party:payer_id", "txn:amount", "txn:currency", "ext:product_name"]
    sql = (
        "SELECT "
        + ", ".join(q(column) for column in columns)
        + " FROM "
        + table_scan_sql(
            connection=CONFIG.connection,
            namespace=CONFIG.namespace,
            table=CONFIG.table,
            columns=columns,
            start_row=start,
            stop_row=stop,
        )
        + " LIMIT 10"
    )
    cursor = scan_ready.execute(sql)
    assert [item[0] for item in cursor.description] == columns
    rows = cursor.fetchall()
    assert len(rows) == 10
    for row in rows:
        # 支出方向付款方是热客户 U000001；收入方向则是商户（M...），两者都是合法 payer
        assert row[0][:1] in {"U", "M"} and row[0][1:].isdigit(), row[0]
        assert Decimal(row[1]) > 0
        assert row[2] in {"CNY", "USD", "EUR", "HKD", "JPY"}
        assert row[3]


def test_first_day_matches_generator(scan_ready, manifest):
    """IT-EXT-09：首日全量结果与确定性 mock 数据逐行一致（多重集合比较）。"""
    dataset = dataset_from_manifest(manifest)
    hot = dataset.hot_user_id
    day_index = 0
    start, stop = helpers.day_range(manifest, day_index)
    expected_count = dataset.hot_day_counts[day_index]

    sql = (
        "SELECT "
        + ", ".join(q(column) for column in ALL_COLUMNS)
        + " FROM "
        + table_scan_sql(
            connection=CONFIG.connection,
            namespace=CONFIG.namespace,
            table=CONFIG.table,
            columns=ALL_COLUMNS,
            start_row=start,
            stop_row=stop,
        )
    )
    rows = scan_ready.execute(sql).fetchall()
    assert len(rows) == expected_count

    expected = Counter()
    for seq in range(1, expected_count + 1):
        record = dataset.row_for(hot, day_index, seq)
        expected[tuple(
            None if record[column] == schema.NULL_TOKEN else record[column]
            for column in ALL_COLUMNS
        )] += 1

    actual = Counter(tuple(row) for row in rows)
    assert actual == expected


def test_day_filter_and_count(scan_ready, manifest):
    """IT-EXT-05：按日范围过滤，count 与规划行数一致（首日/末日）。"""
    dataset = dataset_from_manifest(manifest)
    for day_index in (0, dataset.config.days - 1):
        start, stop = helpers.day_range(manifest, day_index)
        sql = "SELECT count(*) FROM " + table_scan_sql(
            connection=CONFIG.connection,
            namespace=CONFIG.namespace,
            table=CONFIG.table,
            columns=["party:payer_id"],
            start_row=start,
            stop_row=stop,
        )
        assert scan_ready.scalar(sql) == dataset.hot_day_counts[day_index]


def test_currency_group_by(scan_ready, manifest):
    """IT-EXT-06：按币种聚合，结果与 mock 数据一致（首日）。"""
    dataset = dataset_from_manifest(manifest)
    day_index = 0
    start, stop = helpers.day_range(manifest, day_index)
    sql = (
        "SELECT "
        + q("txn:currency")
        + ", count(*) FROM "
        + table_scan_sql(
            connection=CONFIG.connection,
            namespace=CONFIG.namespace,
            table=CONFIG.table,
            columns=["txn:currency"],
            start_row=start,
            stop_row=stop,
        )
        + " GROUP BY 1 ORDER BY 1"
    )
    actual = {code: int(total) for code, total in scan_ready.fetchall(sql)}

    expected: Counter = Counter()
    for seq in range(1, dataset.hot_day_counts[day_index] + 1):
        expected[dataset.row_for(dataset.hot_user_id, day_index, seq)["txn:currency"]] += 1
    assert actual == dict(expected)


def test_limit_is_applied(scan_ready, manifest):
    """IT-EXT-07：LIMIT 只返回指定行数。"""
    start, stop = helpers.hot_range(manifest)
    inner = table_scan_sql(
        connection=CONFIG.connection,
        namespace=CONFIG.namespace,
        table=CONFIG.table,
        columns=["party:payer_id"],
        start_row=start,
        stop_row=stop,
    )
    assert scan_ready.scalar(f"SELECT count(*) FROM (SELECT * FROM {inner} LIMIT 7)") == 7


def test_null_versus_empty_string(scan_ready, manifest):
    """IT-EXT-10：Cell 缺失 -> NULL；Cell 存在但空串 -> ''。

    两个约束：
    1. 必须同时请求一个必然存在的列（party:payer_id），否则 HBase 会跳过
       没有任何匹配 cell 的行，NULL 无从观测；
    2. 当前版本在 scan 结果上直接 FILTER(IS NULL) 存在已知缺陷（见 docs/roadmap.md
       技术债 #1），因此先物化再统计，验证的是 HBase -> DuckDB 的 NULL 传播本身。
    """
    start, stop = helpers.day_range(manifest, 0)
    scan = table_scan_sql(
        connection=CONFIG.connection,
        namespace=CONFIG.namespace,
        table=CONFIG.table,
        columns=["party:payer_id", "ext:remark"],
        start_row=start,
        stop_row=stop,
    )
    scan_ready.execute("DROP TABLE IF EXISTS it_null_probe")
    scan_ready.execute(
        f"CREATE TABLE it_null_probe AS SELECT \"party:payer_id\" AS payer, "
        f"\"ext:remark\" AS remark FROM {scan}"
    )
    try:
        nulls, empties, total = scan_ready.fetchone(
            "SELECT count(*) FILTER (WHERE remark IS NULL), "
            "count(*) FILTER (WHERE remark = ''), count(*) FROM it_null_probe"
        )
    finally:
        scan_ready.execute("DROP TABLE IF EXISTS it_null_probe")

    assert total > 0
    assert (nulls or 0) > 0, "缺少 NULL 语义：应存在 remark 缺失的行"
    assert (empties or 0) > 0, "缺少空串语义：应存在 remark 为空串的行"


def test_pagination_by_day_covers_hot_customer(scan_ready, manifest):
    """IT-EXT-07b：按天分页（stop_row 推进）覆盖全部热客户数据，无重复/遗漏。"""
    dataset = dataset_from_manifest(manifest)
    total = 0
    for day_index in range(dataset.config.days):
        start, stop = helpers.day_range(manifest, day_index)
        sql = "SELECT count(*) FROM " + table_scan_sql(
            connection=CONFIG.connection,
            namespace=CONFIG.namespace,
            table=CONFIG.table,
            columns=["party:payer_id"],
            start_row=start,
            stop_row=stop,
        )
        total += scan_ready.scalar(sql)
    assert total == dataset.config.hot_rows
