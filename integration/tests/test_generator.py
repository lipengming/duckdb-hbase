"""生成器与 Schema 的单元测试（不需要 HBase / DuckDB）。

这些用例守住 mock 数据本身的正确性：RowKey 规则、三档行数、中文内容、
时间跨度、空值语义、确定性与 manifest 抽样。
"""

from __future__ import annotations

import gzip
import re
from datetime import datetime, timedelta
from pathlib import Path

import pytest

from duckdb_hbase_it import schema
from duckdb_hbase_it.generator import (
    BASE_TZ,
    CURRENCIES,
    HOT_TIERS,
    PRODUCTS,
    Dataset,
    DatasetConfig,
    distribute,
    generate,
)
from duckdb_hbase_it.manifest import build_manifest, mark_loaded, read_manifest, write_manifest

PRODUCT_FLAGS = {product_id: has_logistics for product_id, _, has_logistics in PRODUCTS}
CURRENCY_CODES = {code for code, _ in CURRENCIES}
ROWKEY_PATTERN = re.compile(r"^([0-9a-f]{2})(U\d{6})#(\d{8})#(\d{6})$")
AMOUNT_PATTERN = re.compile(r"^\d+\.\d{2}$")


def make_dataset(**overrides) -> Dataset:
    config = dict(
        total_rows=6_000,
        hot_rows=1_200,
        days=30,
        base_date="2025-09-01",
        seed=20250901,
        ordinary_customers=12,
    )
    config.update(overrides)
    return Dataset(DatasetConfig(**config))


def iter_records(dataset: Dataset, limit: int | None = None):
    for index, values in enumerate(dataset.iter_rows()):
        if limit is not None and index >= limit:
            return
        yield dict(zip(schema.COLUMN_SPECS, values[1:])), values[0]


# ---------------------------------------------------------------------------
# Schema / RowKey
# ---------------------------------------------------------------------------


def test_schema_layout_and_rowkey():
    assert schema.FAMILIES == ("party", "txn", "ext")
    assert len(schema.COLUMN_SPECS) == 15
    assert len(schema.split_keys()) == schema.SALT_BUCKETS - 1

    user = "U000123"
    salt = schema.salt_for(user)
    assert re.fullmatch(r"[0-9a-f]{2}", salt)
    assert int(salt, 16) < schema.SALT_BUCKETS
    # 同一个 user_id 的前缀稳定且可连续扫描
    assert schema.salt_for(user) == salt

    key = schema.rowkey(user, "20250915", 42)
    matched = ROWKEY_PATTERN.fullmatch(key)
    assert matched is not None
    assert matched.group(2) == user
    assert matched.group(3) == "20250915"
    assert matched.group(4) == "000042"
    assert key.startswith(salt + user + "#")

    prefix, stop = schema.user_range(user)
    assert prefix == f"{salt}{user}#"
    assert stop == f"{salt}{user}$"
    assert prefix < key < stop

    # 相邻客户不会落入同一前缀范围
    other = "U000124"
    assert not schema.rowkey(other, "20250915", 1).startswith(prefix)


def test_distribute_and_three_tiers():
    for total in (100_000, 500_000, 1_000_000):
        per_day = distribute(total, 30)
        assert len(per_day) == 30
        assert sum(per_day) == total
        assert max(per_day) - min(per_day) <= 1

        # 三档都按「单客户全部数据」构造，验证规划行数不需要真的生成 1M 行
        dataset = Dataset(
            DatasetConfig(
                total_rows=total,
                hot_rows=total,
                days=30,
                base_date="2025-09-01",
                seed=1,
                ordinary_customers=10,
            )
        )
        assert dataset.total_rows == total
        assert sum(dataset.hot_day_counts) == total
        assert sum(dataset.ordinary_row_counts) == 0
        assert dataset.describe()["date_range"] == ["2025-09-01", "2025-09-30"]


def test_validation_rejects_invalid_configs():
    with pytest.raises(ValueError, match="hot_rows"):
        DatasetConfig(total_rows=100, hot_rows=200).validate()
    with pytest.raises(ValueError, match="days"):
        DatasetConfig(days=32).validate()
    with pytest.raises(ValueError, match="跨月"):
        DatasetConfig(base_date="2025-09-01", days=31).validate()
    with pytest.raises(ValueError, match=">= days"):
        DatasetConfig(total_rows=30, hot_rows=10, days=30).validate()


# ---------------------------------------------------------------------------
# 数据内容
# ---------------------------------------------------------------------------


def test_rows_are_chinese_consistent_and_bounded():
    dataset = make_dataset()
    rows = list(iter_records(dataset, limit=800))
    assert rows, "数据为空"

    base = datetime.strptime(dataset.config.base_date, "%Y-%m-%d")
    last_day = (base + timedelta(days=dataset.config.days - 1)).date().isoformat()
    seen_status = set()
    seen_currency = set()

    for record, key in rows:
        assert ROWKEY_PATTERN.fullmatch(key), key
        assert record["party:direction"] in {"支出", "收入"}
        assert record["party:payer_id"] and record["party:payee_id"]
        assert record["party:payer_name"] and record["party:payee_name"]
        # 一笔交易一方是客户（U 开头）、一方是商户（M 开头），且方向与客户所在侧一致
        payer_is_customer = record["party:payer_id"].startswith("U")
        payee_is_customer = record["party:payee_id"].startswith("U")
        assert payer_is_customer != payee_is_customer
        if record["party:payer_id"] == dataset.hot_user_id:
            assert record["party:direction"] == "支出"
        if record["party:payee_id"] == dataset.hot_user_id:
            assert record["party:direction"] == "收入"
        assert base.date().isoformat() <= record["txn:txn_date"] <= last_day
        assert record["txn:txn_time"].startswith(record["txn:txn_date"])
        assert AMOUNT_PATTERN.match(record["txn:amount"]), record["txn:amount"]
        assert float(record["txn:amount"]) > 0
        assert record["txn:currency"] in CURRENCY_CODES
        assert record["ext:product_id"] in PRODUCT_FLAGS
        assert record["ext:product_name"]
        seen_currency.add(record["txn:currency"])
        seen_status.add(record["txn:status"])

        # 有物流单号 => 产品必须有物流，且不是退款单
        logistics = record["ext:logistics_no"]
        if logistics != schema.NULL_TOKEN:
            assert PRODUCT_FLAGS[record["ext:product_id"]], record["ext:product_id"]
            assert record["txn:status"] != "退款"
            assert re.fullmatch(r"(SF|ZTO|YTO|JD|EMS)\d{12}", logistics)
        # 备注三态：文本 / 空串 / 缺失占位
        assert record["ext:remark"] == "" or record["ext:remark"] == schema.NULL_TOKEN or len(
            record["ext:remark"]
        ) > 0

        for value in record.values():
            assert "\t" not in value and "\n" not in value

    # 800 行足以覆盖多个币种与状态
    assert len(seen_currency) >= 2
    assert seen_status <= {"成功", "退款", "处理中"}


def test_txn_ts_matches_txn_time():
    dataset = make_dataset()
    for record, _ in list(iter_records(dataset, limit=50)):
        moment = datetime.strptime(record["txn:txn_time"], "%Y-%m-%d %H:%M:%S").replace(
            tzinfo=BASE_TZ
        )
        assert int(record["txn:txn_ts"]) == int(moment.timestamp() * 1000)


def test_null_and_empty_value_semantics_present():
    dataset = make_dataset(total_rows=4_000, hot_rows=2_000, ordinary_customers=20)
    null_remark = null_logistics = empty_remark = False
    for record, _ in iter_records(dataset, limit=2_000):
        if record["ext:remark"] == schema.NULL_TOKEN:
            null_remark = True
        if record["ext:remark"] == "":
            empty_remark = True
        if record["ext:logistics_no"] == schema.NULL_TOKEN:
            null_logistics = True
    assert null_remark and null_logistics and empty_remark


def test_customer_day_counts_and_ordinary_split():
    dataset = make_dataset(total_rows=1_000, hot_rows=100, ordinary_customers=10)
    assert sum(dataset.ordinary_row_counts) == 900
    assert dataset.ordinary_row_counts == [90] * 10
    assert sum(dataset.hot_day_counts) == 100
    assert dataset.day_counts_for(dataset.hot_user_id) == dataset.hot_day_counts

    # 每行的日期必须落在那一天对应的行数桶里
    for day_index, count in enumerate(dataset.hot_day_counts):
        for seq in (1, count):
            record = dataset.row_for(dataset.hot_user_id, day_index, seq)
            assert record["txn:txn_date"] == dataset.day_iso[day_index]


# ---------------------------------------------------------------------------
# 生成 / manifest
# ---------------------------------------------------------------------------


def test_generation_is_deterministic(tmp_path: Path):
    config = DatasetConfig(
        total_rows=3_000,
        hot_rows=900,
        days=30,
        seed=42,
        ordinary_customers=10,
    )
    first = generate(config, tmp_path / "a.tsv.gz", progress=False)
    second = generate(config, tmp_path / "b.tsv.gz", progress=False)
    assert first.rows == second.rows == 3_000
    assert first.sha256 == second.sha256

    with gzip.open(first.path, "rt", encoding="utf-8") as handle:
        header = handle.readline().rstrip("\n").split("\t")
        assert header[0] == schema.ROWKEY_COLUMN
        assert header[1:] == list(schema.COLUMN_SPECS)
        assert sum(1 for _ in handle) == 3_000


def test_manifest_roundtrip_and_samples(tmp_path: Path):
    config = DatasetConfig(
        total_rows=3_000,
        hot_rows=900,
        days=30,
        seed=7,
        ordinary_customers=10,
    )
    result = generate(config, tmp_path / "data.tsv.gz", progress=False)
    dataset = Dataset(config)
    manifest = build_manifest(dataset, result)
    path = tmp_path / "manifest.json"
    write_manifest(path, manifest)

    loaded = read_manifest(path)
    assert loaded["dataset"]["hot_rows"] == 900
    assert loaded["counts"]["hot_prefix"] == schema.user_prefix(dataset.hot_user_id)
    assert len(loaded["samples"]) >= 8

    seen_rowkeys = set()
    for sample in loaded["samples"]:
        rowkey = sample["rowkey"]
        assert rowkey not in seen_rowkeys
        seen_rowkeys.add(rowkey)
        expected = dataset.row_for(sample["user_id"], sample["day_index"], sample["seq"])
        assert expected[schema.ROWKEY_COLUMN] == rowkey
        for column, value in sample["cells"].items():
            assert expected[column] == value
        for column in sample["null_cells"]:
            assert expected[column] == schema.NULL_TOKEN

    updated = mark_loaded(path, {"rows": 3_000, "cells": 42_000, "elapsed_ms": 123})
    assert updated["loaded"]["rows"] == 3_000
    assert read_manifest(path)["loaded"]["rows"] == 3_000


def test_existing_file_requires_force(tmp_path: Path):
    config = DatasetConfig(total_rows=200, hot_rows=120, days=30, ordinary_customers=2)
    generate(config, tmp_path / "data.tsv.gz", progress=False)
    with pytest.raises(FileExistsError):
        generate(config, tmp_path / "data.tsv.gz", progress=False)
    result = generate(config, tmp_path / "data.tsv.gz", progress=False, force=True)
    assert result.rows == 200
