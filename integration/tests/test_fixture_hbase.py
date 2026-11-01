"""数据面集成用例（只依赖 HBase，不需要 DuckDB / Extension）。

用途：在 Extension 尚未接入或排查问题时，先证明 HBase 夹具本身正确：
表结构、预分区、总行数、单客户三档行数、RowKey 布局、时间跨度、
收付款方/币种/产品/物流/备注的取值语义、NULL 与空串的区分。

对应 ``docs/testing.md`` §5.2「测试数据准备」与 §5.3「端到端断言」的数据前置。
"""

from __future__ import annotations

import re
from datetime import datetime
from decimal import Decimal

import pytest

from duckdb_hbase_it import loader, schema
from duckdb_hbase_it.generator import BASE_TZ, PRODUCTS

import it_helpers as helpers

pytestmark = pytest.mark.dataplane

ROWKEY_PATTERN = re.compile(r"^([0-9a-f]{2})(U\d{6})#(\d{8})#(\d{6})$")
PRODUCT_FLAGS = {product_id: has_logistics for product_id, _, has_logistics in PRODUCTS}
CURRENCY_CODES = {"CNY", "USD", "EUR", "HKD", "JPY"}


def test_table_exists_with_expected_regions(manifest, hbase_table):
    """IT-DATA-01：命名空间/表存在，预分区数 = salt 桶数（16）。"""
    assert hbase_table["exists"] is True
    assert hbase_table["regions"] == manifest["table"]["salt_buckets"]
    assert manifest["table"]["families"] == list(schema.FAMILIES)


def test_single_customer_row_count(manifest):
    """IT-DATA-03：单客户（热客户）行数 == manifest 档位。"""
    start, stop = helpers.hot_range(manifest)
    result = loader.count(start=start, stop=stop)
    assert result["count"] == manifest["counts"]["hot_rows"]


@pytest.mark.slow
def test_total_row_count(manifest):
    """IT-DATA-02：全表行数 == 1,000,000（默认配置）。"""
    result = loader.count()
    assert result["count"] == manifest["counts"]["total_rows"]


@pytest.mark.slow
def test_each_day_has_expected_rows(manifest):
    """IT-DATA-04：交易时间跨度覆盖 30 天，且每天行数与规划一致。"""
    counts = manifest["counts"]["hot_day_counts"]
    actual = []
    for day_index in range(len(counts)):
        start, stop = helpers.day_range(manifest, day_index)
        actual.append(loader.count(start=start, stop=stop)["count"])
    assert actual == counts
    assert sum(actual) == manifest["counts"]["hot_rows"]


def test_rowkey_layout_and_order(manifest):
    """IT-DATA-05：RowKey = salt + user_id#yyyymmdd#seq，且按字节序单调。"""
    prefix, stop = helpers.hot_range(manifest)
    rows = loader.scan(
        start=prefix,
        stop=stop,
        limit=500,
        columns=["party:payer_id", "txn:txn_date"],
    )
    assert len(rows) == 500

    dates = manifest["counts"]["date_range"]
    previous_key = ""
    for row in rows:
        matched = ROWKEY_PATTERN.fullmatch(row["rowkey"])
        assert matched, row["rowkey"]
        salt, user_id, day, seq = matched.groups()
        assert user_id == manifest["counts"]["hot_user_id"]
        assert salt == schema.salt_for(user_id)
        assert row["rowkey"].startswith(prefix)
        assert row["rowkey"] > previous_key
        previous_key = row["rowkey"]
        assert dates[0] <= row["txn:txn_date"] <= dates[1]
        # seq 从 1 开始且当天内连续
        assert int(seq) >= 1
        assert row["party:payer_id"] == user_id or row["party:payer_id"].startswith("M")


def test_prefix_range_isolation(manifest):
    """IT-DATA-06：单客户前缀范围不串号；范围外第一条不是该客户。"""
    prefix, stop = helpers.hot_range(manifest)
    rows = loader.scan(start=prefix, stop=stop, limit=2_000, columns=["party:payer_id"])
    assert all(row["rowkey"].startswith(prefix) for row in rows)
    assert len({row["rowkey"] for row in rows}) == len(rows)

    after = loader.scan(start=stop, limit=1, columns=["party:payer_id"])
    if after:
        assert not after[0]["rowkey"].startswith(prefix)


def test_party_amount_currency_semantics(manifest):
    """IT-DATA-07/08：收付款方、方向、金额、币种、产品与物流的取值语义。"""
    prefix, stop = helpers.hot_range(manifest)
    columns = [
        "party:payer_id",
        "party:payer_name",
        "party:payee_id",
        "party:payee_name",
        "party:direction",
        "txn:amount",
        "txn:currency",
        "txn:txn_time",
        "txn:txn_ts",
        "txn:txn_date",
        "txn:status",
        "ext:product_id",
        "ext:product_name",
        "ext:logistics_no",
        "ext:remark",
    ]
    rows = loader.scan(start=prefix, stop=stop, limit=300, columns=columns)
    assert len(rows) == 300

    for row in rows:
        assert row["party:payer_id"] and row["party:payer_name"]
        assert row["party:payee_id"] and row["party:payee_name"]
        assert row["party:direction"] in ("支出", "收入")
        payer_is_customer = row["party:payer_id"].startswith("U")
        payee_is_customer = row["party:payee_id"].startswith("U")
        assert payer_is_customer != payee_is_customer
        assert Decimal(row["txn:amount"]) > 0
        assert row["txn:currency"] in CURRENCY_CODES
        assert row["ext:product_id"] in PRODUCT_FLAGS
        assert row["ext:product_name"]

        moment = datetime.strptime(row["txn:txn_time"], "%Y-%m-%d %H:%M:%S").replace(
            tzinfo=BASE_TZ
        )
        assert int(row["txn:txn_ts"]) == int(moment.timestamp() * 1000)
        assert row["txn:txn_date"] == row["txn:txn_time"][:10]

        logistics = row["ext:logistics_no"]
        if logistics is not None:
            assert PRODUCT_FLAGS[row["ext:product_id"]]
            assert row["txn:status"] != "退款"
            assert re.fullmatch(r"(SF|ZTO|YTO|JD|EMS)\d{12}", logistics)


def test_manifest_samples_roundtrip(manifest):
    """IT-DATA-09：manifest 抽样行在 HBase 中的单元格值完全一致（含 NULL/空串）。"""
    samples = manifest["samples"]
    assert samples
    fetched = {
        row["rowkey"]: row for row in loader.scan(rowkeys=[sample["rowkey"] for sample in samples])
    }
    problems = []
    for sample in samples:
        row = fetched.get(sample["rowkey"])
        if row is None:
            problems.append(f"{sample['rowkey']} 不存在")
            continue
        problems.extend(helpers.record_matches(row, sample))
    assert not problems, "\n".join(problems)


def test_null_versus_empty_string(manifest):
    """IT-DATA-10：Cell 缺失读回 None；Cell 存在但空串保持 ""。"""
    samples = manifest["samples"]
    expected = [sample for sample in samples if "ext:remark" in sample.get("null_cells", [])]
    empty = [
        sample
        for sample in samples
        if sample["cells"].get("ext:remark") == ""
    ]
    assert expected, "manifest 缺少 remark 缺失样本"
    assert empty, "manifest 缺少 remark 空串样本"

    fetched = {
        row["rowkey"]: row
        for row in loader.scan(rowkeys=[sample["rowkey"] for sample in expected + empty])
    }
    for sample in expected:
        assert fetched[sample["rowkey"]]["ext:remark"] is None
    for sample in empty:
        assert fetched[sample["rowkey"]]["ext:remark"] == ""


def test_sample_row_values_are_chinese(manifest):
    """IT-DATA-11：中文内容未被编码破坏（名称/产品/备注/状态）。"""
    row = manifest["samples"][0]
    fetched = loader.scan(rowkeys=[row["rowkey"]])[0]
    for column in ("party:payer_name", "ext:product_name"):
        assert fetched[column] == row["cells"][column]
        assert any("\u4e00" <= char <= "\u9fff" for char in fetched[column])
