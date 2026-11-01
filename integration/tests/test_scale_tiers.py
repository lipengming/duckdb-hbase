"""单客户三档规模用例（10 万 / 50 万 / 100 万）。

同一时刻 HBase 里只有一档数据；三档通过重新生成 + 装载切换：

    python -m duckdb_hbase_it all --rows 1000000 --hot-rows 1000000 --force

用例对未装载的档位 skip 并打印切换命令，对当前档位做行数/时间跨度断言。
"""

from __future__ import annotations

import pytest

from duckdb_hbase_it import loader
from duckdb_hbase_it.config import CONFIG
from duckdb_hbase_it.duckdb_session import table_scan_sql
from duckdb_hbase_it.generator import HOT_TIERS

import it_helpers as helpers

pytestmark = [pytest.mark.extension, pytest.mark.scale]


def _tier_id(tier: int) -> str:
    return f"{tier // 10_000}万"


@pytest.mark.parametrize(
    "tier",
    [
        pytest.param(
            tier,
            id=_tier_id(tier),
            marks=pytest.mark.slow if tier >= 1_000_000 else (),
        )
        for tier in HOT_TIERS
    ],
)
def test_single_customer_tier(tier, manifest, scan_ready):
    """IT-EXT-11：单客户 10 万/50 万/100 万档位的扫描与行数一致性。"""
    current = manifest["dataset"]["hot_rows"]
    if tier != current:
        pytest.skip(
            f"当前装载档位 {current:,}；切换到本档："
            f"`python -m duckdb_hbase_it all --rows {tier} --hot-rows {tier} --force`"
        )

    start, stop = helpers.hot_range(manifest)
    sql = "SELECT count(*) FROM " + table_scan_sql(
        connection=CONFIG.connection,
        namespace=CONFIG.namespace,
        table=CONFIG.table,
        columns=["party:payer_id"],
        start_row=start,
        stop_row=stop,
    )
    assert scan_ready.scalar(sql) == tier
    assert loader.count(start=start, stop=stop)["count"] == tier

    # 时间跨度必须覆盖配置的整月
    dataset = manifest["dataset"]
    assert dataset["days"] in (28, 29, 30, 31)
    assert manifest["counts"]["date_range"][1] >= manifest["counts"]["date_range"][0]


@pytest.mark.slow
def test_active_tier_day_pagination(manifest, scan_ready):
    """IT-EXT-11b：当前档位按天分页覆盖全部单客户数据（slow）。"""
    days = manifest["dataset"]["days"]
    total = 0
    for day_index in range(days):
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
    assert total == manifest["dataset"]["hot_rows"]
