"""测试侧的公共小工具（不依赖 pytest）。"""

from __future__ import annotations

from typing import Dict, Iterable, List, Optional

from duckdb_hbase_it import schema


def hot_range(manifest: Dict) -> tuple:
    counts = manifest["counts"]
    return counts["hot_range"][0], counts["hot_range"][1]


def hot_prefix(manifest: Dict) -> str:
    return manifest["counts"]["hot_prefix"]


def hot_user_id(manifest: Dict) -> str:
    return manifest["counts"]["hot_user_id"]


def day_prefix(manifest: Dict, day_index: int) -> str:
    base = manifest["dataset"]["base_date"]
    year, month, day = (int(part) for part in base.split("-"))
    from datetime import date, timedelta

    current = date(year, month, day) + timedelta(days=day_index)
    return f"{hot_prefix(manifest)}{current.strftime('%Y%m%d')}"


def day_range(manifest: Dict, day_index: int) -> tuple:
    """单客户单日的左闭右开 RowKey 范围（末位前缀 + ``$``）。"""
    prefix = day_prefix(manifest, day_index)
    return prefix, prefix[:-1] + chr(ord(prefix[-1]) + 1)


def sample_by_column(manifest: Dict, column: str) -> Optional[Dict]:
    for sample in manifest.get("samples", []):
        if column in sample["cells"]:
            return sample
    return None


def sample_with_null(manifest: Dict, column: str) -> Optional[Dict]:
    for sample in manifest.get("samples", []):
        if column in sample.get("null_cells", []):
            return sample
    return None


def record_matches(record: Dict[str, Optional[str]], expected: Dict) -> List[str]:
    """比对一行：返回差异描述列表（空列表表示一致）。"""
    problems: List[str] = []
    for column, value in expected["cells"].items():
        if record.get(column) != value:
            problems.append(f"{column}: 期望 {value!r}，实际 {record.get(column)!r}")
    for column in expected.get("null_cells", []):
        if record.get(column) is not None:
            problems.append(f"{column}: 期望 NULL，实际 {record.get(column)!r}")
    return problems
