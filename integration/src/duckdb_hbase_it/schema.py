"""客户交易宽表的 Schema、RowKey 规则与列常量。

RowKey 设计（对齐需求「user_id + date(到日)，前缀通过 user_id 打散」）：

    <salt:2hex><user_id>#<yyyymmdd>#<seq:06d>
    例：0aU000001#20250901#000042

- ``salt`` = ``crc32(user_id) % 16``，两位小写十六进制，用于把不同客户打散到 16 个预分区；
  因为 salt 由 user_id 决定，同一客户的所有行仍然连续，可继续用前缀范围扫描。
- ``date`` 精确到日，用于按天聚合与按天范围扫描。
- ``seq`` 是「同一客户同一天」内的序号（6 位零填充）。仅用 user_id+date 无法容纳
  单客户 10 万/50 万/100 万行（一个月最多 31 个不同 rowkey），因此增加确定性序号作为
  同一天内的唯一后缀；这是对需求 RowKey 的**必要扩展**，README「RowKey 设计」有说明。

列族划分：

- ``party``：收付款方（id + 名称）与方向；
- ``txn``：交易时间、日期、金额、币种、状态；
- ``ext``：产品、物流单号、备注。
"""

from __future__ import annotations

import zlib
from typing import Dict, List, Tuple

NAMESPACE = "duckdb_test"
TABLE = "customer_transaction"

#: 预分区数量，与 salt 的 16 个取值一一对应。
SALT_BUCKETS = 16

#: 列族（均为单版本；宽表不使用多版本语义）。
FAMILIES: Tuple[str, ...] = ("party", "txn", "ext")

#: 列定义，顺序即 TSV 列顺序，也是 Extension 默认投影顺序。
#: 元素为 ``(family, qualifier)``。
COLUMNS: Tuple[Tuple[str, str], ...] = (
    ("party", "payer_id"),
    ("party", "payer_name"),
    ("party", "payee_id"),
    ("party", "payee_name"),
    ("party", "direction"),
    ("txn", "txn_time"),
    ("txn", "txn_ts"),
    ("txn", "txn_date"),
    ("txn", "amount"),
    ("txn", "currency"),
    ("txn", "status"),
    ("ext", "product_id"),
    ("ext", "product_name"),
    ("ext", "logistics_no"),
    ("ext", "remark"),
)

#: ``family:qualifier`` 形式的列名，例如 ``party:payer_id``。
COLUMN_SPECS: Tuple[str, ...] = tuple(f"{family}:{qualifier}" for family, qualifier in COLUMNS)

#: 空值占位符：装载器遇到该值时不写该 Cell（读回即 NULL），
#: 从而可以区分「Cell 不存在（NULL）」与「Cell 存在但值为空字符串」。
NULL_TOKEN = r"\N"

#: 表头第一列名。
ROWKEY_COLUMN = "rowkey"


def salt_for(user_id: str) -> str:
    """返回 user_id 对应的 2 位十六进制前缀（打散用，同一 user_id 恒定）。"""
    return f"{zlib.crc32(user_id.encode('utf-8')) % SALT_BUCKETS:02x}"


def rowkey(user_id: str, day: "str", seq: int) -> str:
    """构造 RowKey；``day`` 为 ``yyyymmdd`` 字符串，``seq`` 从 1 开始。"""
    return f"{salt_for(user_id)}{user_id}#{day}#{seq:06d}"


def user_prefix(user_id: str) -> str:
    """单客户前缀（含 salt 与分隔符），用于前缀范围扫描。"""
    return f"{salt_for(user_id)}{user_id}#"


def user_range(user_id: str) -> Tuple[str, str]:
    """单客户 RowKey 左闭右开范围 ``[prefix, prefix_upper)``。

    由于前缀末位是 ``#``（0x23），下一位字符只有数字/``$`` 等更大字符，
    把末位加一得到 ``$``，即可构成严格上界，不会误伤相邻客户。
    """
    prefix = user_prefix(user_id)
    stop = prefix[:-1] + chr(ord(prefix[-1]) + 1)
    return prefix, stop


def day_prefix(user_id: str, day: str) -> str:
    """单客户单日前缀，用于按天扫描/计数。"""
    return f"{user_prefix(user_id)}{day}"


def split_keys() -> List[str]:
    """16 个预分区的 split key：``01`` .. ``0f``（RowKey 为 ASCII 十六进制前缀）。"""
    return [f"{i:02x}" for i in range(1, SALT_BUCKETS)]


def columns_of_family(family: str) -> List[str]:
    """返回某列族下的 qualifier 列表。"""
    return [qualifier for fam, qualifier in COLUMNS if fam == family]


def column_index(spec: str) -> int:
    """``family:qualifier`` 在 COLUMNS 中的下标，便于按位置取值。"""
    try:
        return COLUMN_SPECS.index(spec)
    except ValueError as exc:  # pragma: no cover - 防御性
        raise KeyError(f"未知列: {spec}") from exc


def columns_by_family() -> Dict[str, List[str]]:
    result: Dict[str, List[str]] = {family: [] for family in FAMILIES}
    for family, qualifier in COLUMNS:
        result[family].append(qualifier)
    return result
