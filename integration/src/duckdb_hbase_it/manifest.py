"""数据集 manifest：把「生成了什么、装载了什么、抽样长什么样」固化成 JSON。

manifest 是测试与数据之间的契约：

- pytest 只根据 manifest 断言行数、RowKey 前缀、抽样单元格，不重复解析 TSV；
- ``load`` 完成后把装载统计回写到 manifest（原子替换），保证测试看到的是真实装载结果；
- 记录了生成参数的 seed 与 sha256，便于复现与排查。
"""

from __future__ import annotations

import json
import os
import platform
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional

from . import __version__, schema
from .generator import Dataset, GenerationResult

SCHEMA_VERSION = 1


def _sample_cells(dataset: Dataset, user_id: str, day_index: int, seq: int) -> Dict[str, Any]:
    record = dataset.row_for(user_id, day_index, seq)
    rowkey = record.pop(schema.ROWKEY_COLUMN)
    cells = {spec: value for spec, value in record.items() if value != schema.NULL_TOKEN}
    return {
        "rowkey": rowkey,
        "user_id": user_id,
        "day_index": day_index,
        "seq": seq,
        "cells": cells,
        "null_cells": sorted(spec for spec, value in record.items() if value == schema.NULL_TOKEN),
    }


def _first_matching_row(
    dataset: Dataset, predicate, *, limit: int = 20_000
) -> Optional[Dict[str, Any]]:
    """在热客户前 ``limit`` 行中找第一行满足谓词的数据，用于覆盖空值/空串语义。"""
    hot = dataset.hot_user_id
    checked = 0
    for day_index, count in enumerate(dataset.hot_day_counts):
        for seq in range(1, count + 1):
            record = dataset.row_for(hot, day_index, seq)
            checked += 1
            if predicate(record):
                return _sample_cells(dataset, hot, day_index, seq)
            if checked >= limit:
                return None
    return None



def _last_nonzero_index(counts: List[int]) -> int:
    for index in range(len(counts) - 1, -1, -1):
        if counts[index] > 0:
            return index
    raise ValueError("day counts 全为 0")


def build_samples(dataset: Dataset) -> List[Dict[str, Any]]:
    """挑选确定性抽样行：覆盖月初/月末、热客户/普通客户、NULL/空串。"""
    config = dataset.config
    hot = dataset.hot_user_id
    mid = config.days // 2
    samples: List[Dict[str, Any]] = []

    specs = [
        (hot, 0, 1),
        (hot, 0, dataset.hot_day_counts[0]),
        (hot, mid, 1),
        (hot, mid, dataset.hot_day_counts[mid]),
        (hot, config.days - 1, 1),
        (hot, config.days - 1, dataset.hot_day_counts[-1]),
    ]

    ordinary_with_rows = [
        user_id
        for user_id, count in zip(dataset.ordinary_ids, dataset.ordinary_row_counts)
        if count > 0
    ]
    if ordinary_with_rows:
        first_id = ordinary_with_rows[0]
        last_id = ordinary_with_rows[-1]
        first_counts = dataset.day_counts_for(first_id)
        last_counts = dataset.day_counts_for(last_id)
        first_day = first_counts.index(max(first_counts))
        last_day = _last_nonzero_index(last_counts)
        specs.append((first_id, first_day, 1))
        specs.append((first_id, first_day, first_counts[first_day]))
        specs.append((last_id, last_day, 1))
        specs.append((last_id, last_day, last_counts[last_day]))

    for user_id, day_index, seq in specs:
        if seq <= 0:
            continue
        samples.append(_sample_cells(dataset, user_id, day_index, seq))

    # 空 Cell（读回 NULL）与显式空串各取一行，避免测试拿不到这两种语义。
    null_logistics = _first_matching_row(
        dataset, lambda record: record["ext:logistics_no"] == schema.NULL_TOKEN
    )
    null_remark = _first_matching_row(
        dataset, lambda record: record["ext:remark"] == schema.NULL_TOKEN
    )
    empty_remark = _first_matching_row(dataset, lambda record: record["ext:remark"] == "")

    existing = {sample["rowkey"] for sample in samples}
    for extra in (null_logistics, null_remark, empty_remark):
        if extra and extra["rowkey"] not in existing:
            samples.append(extra)
            existing.add(extra["rowkey"])

    return samples


def build_manifest(dataset: Dataset, result: GenerationResult) -> Dict[str, Any]:
    config = dataset.config
    return {
        "schema_version": SCHEMA_VERSION,
        "package_version": __version__,
        "generated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "host": {"platform": platform.platform(), "python": platform.python_version()},
        "dataset": {
            "total_rows": config.total_rows,
            "hot_rows": config.hot_rows,
            "days": config.days,
            "base_date": config.base_date,
            "seed": config.seed,
            "ordinary_customers": config.ordinary_customers,
        },
        "table": {
            "namespace": config.namespace,
            "name": config.table,
            "families": list(schema.FAMILIES),
            "columns": list(schema.COLUMN_SPECS),
            "salt_buckets": schema.SALT_BUCKETS,
            "split_keys": schema.split_keys(),
            "null_token": schema.NULL_TOKEN,
        },
        "file": {
            "path": str(result.path),
            "sha256": result.sha256,
            "bytes": result.bytes_written,
            "rows": result.rows,
            "elapsed_seconds": round(result.elapsed_seconds, 3),
        },
        "counts": dataset.describe(),
        "samples": build_samples(dataset),
        "loaded": None,
    }


def write_manifest(path: Path, manifest: Dict[str, Any]) -> None:
    """原子写入 manifest（先写临时文件再 rename）。"""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("w", encoding="utf-8") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    os.replace(tmp, path)


def read_manifest(path: Path) -> Dict[str, Any]:
    with Path(path).open("r", encoding="utf-8") as handle:
        manifest = json.load(handle)
    if manifest.get("schema_version") != SCHEMA_VERSION:
        raise ValueError(
            f"manifest schema_version={manifest.get('schema_version')} 不受支持"
        )
    return manifest


def mark_loaded(path: Path, loaded: Dict[str, Any]) -> Dict[str, Any]:
    """回写装载统计并返回更新后的 manifest。"""
    manifest = read_manifest(path)
    manifest["loaded"] = {**loaded, "at": datetime.now(timezone.utc).isoformat(timespec="seconds")}
    write_manifest(path, manifest)
    return manifest


