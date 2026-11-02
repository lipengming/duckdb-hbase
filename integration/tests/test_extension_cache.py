"""缓存语义用例（Full Hit / Partial Hit / Clear）。

``hbase_cache_stats()`` 的输出列在方案中标注为 TBD，因此这些用例对统计列采取
「按列名关键字识别」的宽松策略：识别不到计数列时 skip 并注明 TBD，
而不是写死一个尚未定稿的 schema。结果正确性断言始终执行。
"""

from __future__ import annotations

import pytest

from duckdb_hbase_it import schema
from duckdb_hbase_it.config import CONFIG
from duckdb_hbase_it.duckdb_session import table_scan_sql

import it_helpers as helpers

pytestmark = [pytest.mark.extension, pytest.mark.cache]


def _stats(connection):
    cursor = connection.execute("SELECT * FROM hbase_cache_stats()")
    names = [description[0] for description in cursor.description]
    return names, cursor.fetchall()


def _find_column(names, *keywords):
    for keyword in keywords:
        for index, name in enumerate(names):
            if keyword in name.lower():
                return index
    return None


def _sum_column(rows, index):
    total = 0
    for row in rows:
        value = row[index]
        if value is not None:
            total += int(value)
    return total


def _day_scan(connection, manifest, day_index, columns):
    start, stop = helpers.day_range(manifest, day_index)
    return table_scan_sql(
        connection=CONFIG.connection,
        namespace=CONFIG.namespace,
        table=CONFIG.table,
        columns=columns,
        start_row=start,
        stop_row=stop,
        cache=True,
    )


def test_full_hit_does_not_rescan(scan_ready, manifest):
    """IT-EXT-08：同范围同列重复查询 -> 远端 Scan 增量为 0，结果一致。"""
    if not scan_ready.has_cache_stats():
        pytest.skip("hbase_cache_stats() 尚未实现（TBD），无法观测命中状态")

    scan = _day_scan(scan_ready, manifest, 0, ["party:payer_id", "txn:amount"])
    first = scan_ready.fetchall(f"SELECT count(*), sum(CAST(\"txn:amount\" AS DECIMAL(18,2))) FROM {scan}")
    names, before = _stats(scan_ready)
    second = scan_ready.fetchall(f"SELECT count(*), sum(CAST(\"txn:amount\" AS DECIMAL(18,2))) FROM {scan}")
    _, after = _stats(scan_ready)
    assert first == second

    scan_index = _find_column(names, "scan")
    if scan_index is None:
        pytest.skip(f"cache stats 未暴露 scan 计数列（TBD），当前列：{names}")
    delta = _sum_column(after, scan_index) - _sum_column(before, scan_index)
    assert delta == 0, f"Full Hit 仍触发远端 Scan：{names} delta={delta}"


def test_partial_hit_backfills_columns(scan_ready, manifest):
    """IT-EXT-09：在已缓存列基础上补列 -> 行数不变、新列值正确。"""
    if not scan_ready.has_cache_stats():
        pytest.skip("hbase_cache_stats() 尚未实现（TBD），无法观测补列批次")

    base_columns = ["party:payer_id", "txn:amount"]
    extended_columns = base_columns + ["ext:product_name", "txn:currency"]
    base_scan = _day_scan(scan_ready, manifest, 0, base_columns)
    extended_scan = _day_scan(scan_ready, manifest, 0, extended_columns)

    scan_ready.fetchall(f"SELECT * FROM {base_scan}")
    names, before = _stats(scan_ready)
    rows = scan_ready.fetchall(f"SELECT * FROM {extended_scan}")
    _, after = _stats(scan_ready)

    assert rows
    for row in rows[:50]:
        assert row[0]
        assert row[2]
        assert row[3]

    get_index = _find_column(names, "get")
    if get_index is None:
        pytest.skip(f"cache stats 未暴露 get 计数列（TBD），当前列：{names}")
    delta = _sum_column(after, get_index) - _sum_column(before, get_index)
    assert delta > 0, f"Partial Hit 未发起补列 Get：{names} delta={delta}"


def test_cache_disabled_matches_enabled(scan_ready, manifest):
    """IT-EXT-10：cache := false 与 true 返回相同结果。"""
    start, stop = helpers.day_range(manifest, 0)
    columns = ["party:payer_id", "txn:amount"]

    def scan_sql(cache):
        return table_scan_sql(
            connection=CONFIG.connection,
            namespace=CONFIG.namespace,
            table=CONFIG.table,
            columns=columns,
            start_row=start,
            stop_row=stop,
            cache=cache,
        )

    try:
        scan_ready.fetchall(f"SELECT count(*) FROM {scan_sql(True)}")
    except Exception as exc:  # noqa: BLE001 - 明确区分“未实现”与真实错误
        if "缓存引擎尚未实现" in str(exc):
            pytest.skip("缓存引擎尚未实现（M3）：cache := true 目前会显式报错")
        raise

    with_cache = scan_ready.fetchall(f"SELECT * FROM {scan_sql(True)}")
    without_cache = scan_ready.fetchall(f"SELECT * FROM {scan_sql(False)}")
    assert with_cache == without_cache


def test_cache_clear(scan_ready, manifest):
    """IT-EXT-10b：清理缓存后再次查询仍返回正确结果，且不报错。"""
    if not scan_ready.has_cache_clear():
        pytest.skip("hbase_cache_clear() 尚未实现（TBD）")
    scan = _day_scan(scan_ready, manifest, 0, ["party:payer_id", "txn:amount"])
    before = scan_ready.fetchall(f"SELECT count(*) FROM {scan}")
    scan_ready.execute("SELECT hbase_cache_clear()")
    after = scan_ready.fetchall(f"SELECT count(*) FROM {scan}")
    assert before == after
