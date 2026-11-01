"""确定性「客户交易宽表」mock 数据生成器。

特点：

1. **确定性**：同一 ``seed`` 生成的数据逐字节一致（sha256 可复现），方便断言与回归；
2. **中文内容**：客户/商户名称、产品、备注、状态、方向均使用中文；
3. **单客户三档**：热客户行数由 ``hot_rows`` 控制，支持 10 万 / 50 万 / 100 万；
4. **时间跨度**：默认 2025-09-01 ~ 2025-09-30（30 天，配置化），每行同时写
   ``txn_time``（可读）、``txn_ts``（毫秒时间戳）、``txn_date``（按日聚合）；
5. **空值语义**：缺 Cell 用 ``\\N`` 占位（装载时跳过，读回为 NULL），
   以区分「Cell 不存在」与「Cell 存在但值为空字符串」。

生成结果的列顺序与 :mod:`duckdb_hbase_it.schema` 的 ``COLUMNS`` 严格一致。
"""

from __future__ import annotations

import gzip
import hashlib
import time
from dataclasses import dataclass, field
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
from typing import Dict, Iterator, List, Tuple

from . import schema
from .config import CONFIG, IntegrationConfig

# ---------------------------------------------------------------------------
# 维度数据（中文 mock）
# ---------------------------------------------------------------------------

SURNAMES: Tuple[str, ...] = (
    "赵", "钱", "孙", "李", "周", "吴", "郑", "王", "冯", "陈",
    "褚", "卫", "蒋", "沈", "韩", "杨", "朱", "秦", "尤", "许",
    "何", "吕", "施", "张", "孔", "曹", "严", "华", "金", "魏",
    "陶", "姜", "谢", "邹", "喻", "柏", "窦", "章", "苏", "潘",
    "范", "彭", "鲁", "韦", "马", "苗", "凤", "郝", "方", "俞",
)

GIVEN_NAMES: Tuple[str, ...] = (
    "伟", "芳", "娜", "秀英", "敏", "静", "丽", "强", "磊", "军",
    "洋", "勇", "艳", "杰", "娟", "涛", "明", "超", "秀兰", "霞",
    "平", "刚", "桂英", "建国", "文", "斌", "红", "玉兰", "海燕", "志强",
    "晓明", "雅静", "子轩", "雨涵", "思远", "嘉怡", "浩然", "梓萱", "俊杰", "欣怡",
)

MERCHANT_BASE_NAMES: Tuple[str, ...] = (
    "京东商城", "天猫超市", "拼多多", "苏宁易购", "美团外卖",
    "饿了么", "顺丰速运", "中通快递", "圆通速递", "申通快递",
    "韵达快递", "中国移动", "中国联通", "国家电网", "中石化",
    "中石油", "滴滴出行", "携程旅行", "华住酒店", "海底捞",
    "星巴克", "瑞幸咖啡", "沃尔玛", "家乐福", "永辉超市",
    "盒马鲜生", "小米之家", "华为授权店", "苹果零售店", "迪卡侬",
    "优衣库", "屈臣氏", "大润发", "便利蜂", "菜鸟驿站",
    "中国邮政", "招商银行", "平安保险", "中国国航", "锦江之星",
)

#: (product_id, product_name, 是否有物流单号)
PRODUCTS: Tuple[Tuple[str, str, bool], ...] = (
    ("P0001", "智能扫地机器人 Pro", True),
    ("P0002", "无线蓝牙耳机", True),
    ("P0003", "有机鲜牛奶 1L", True),
    ("P0004", "东北大米 5kg", True),
    ("P0005", "空调 1.5匹", True),
    ("P0006", "笔记本电脑 14英寸", True),
    ("P0007", "运动跑鞋", True),
    ("P0008", "冬季羽绒服", True),
    ("P0009", "儿童绘本套装", True),
    ("P0010", "咖啡豆 500g", True),
    ("P0011", "手机话费充值", False),
    ("P0012", "视频会员年卡", False),
    ("P0013", "云存储扩容包", False),
    ("P0014", "家政保洁服务", False),
    ("P0015", "机票改签服务", False),
    ("P0016", "健身私教课", False),
    ("P0017", "电影票兑换券", False),
    ("P0018", "宽带安装服务", False),
    ("P0019", "智能门锁", True),
    ("P0020", "空气净化器", True),
    ("P0021", "电动牙刷", True),
    ("P0022", "宠物主粮 10kg", True),
    ("P0023", "有机蔬菜礼盒", True),
    ("P0024", "保温杯", True),
    ("P0025", "显示器 27英寸", True),
    ("P0026", "机械键盘", True),
    ("P0027", "摄影三脚架", True),
    ("P0028", "瑜伽垫", True),
    ("P0029", "车载充电器", True),
    ("P0030", "婴儿纸尿裤", True),
    ("P0031", "洗衣液 3kg", True),
    ("P0032", "食用油 5L", True),
    ("P0033", "保险续保服务", False),
    ("P0034", "酒店预订服务", False),
    ("P0035", "在线课程包", False),
    ("P0036", "家政维修上门", False),
)

#: 币种及权重（合计 100）。
CURRENCIES: Tuple[Tuple[str, int], ...] = (
    ("CNY", 88),
    ("USD", 6),
    ("EUR", 3),
    ("HKD", 2),
    ("JPY", 1),
)

CARRIERS: Tuple[str, ...] = ("SF", "ZTO", "YTO", "JD", "EMS")

REMARKS: Tuple[str, ...] = (
    "客户备注：请尽快发货",
    "大促活动订单，优先处理",
    "退款原因：尺码不合适",
    "企业采购，需开具增值税专用发票",
    "客户要求周末送达",
    "重复下单，已与客户确认",
    "礼品包装，附贺卡",
    "跨境订单，需报关",
    "客户自提",
    "货到付款",
    "优惠券抵扣后实付",
    "售后换货",
)

DIRECTION_EXPENSE = "支出"
DIRECTION_INCOME = "收入"
STATUS_SUCCESS = "成功"
STATUS_REFUND = "退款"
STATUS_PENDING = "处理中"

#: +08:00（数据约定时区；避免测试结果随机器本地时区变化）。
BASE_TZ = timezone(timedelta(hours=8))

#: 生成器内部的分档边界（需求要求的三档单客规模）。
HOT_TIERS: Tuple[int, ...] = (100_000, 500_000, 1_000_000)


def _mix(*values: int) -> int:
    """稳定的 32 位整数混合（FNV-1a），跨进程/跨平台一致。"""
    result = 2_166_136_261
    for value in values:
        result ^= value & 0xFFFFFFFF
        result = (result * 16_777_619) & 0xFFFFFFFF
    return result


def distribute(total: int, buckets: int) -> List[int]:
    """把 ``total`` 尽量均匀地分到 ``buckets`` 个桶，余数分给前几个桶。"""
    if buckets <= 0:
        raise ValueError("buckets 必须为正数")
    base, extra = divmod(total, buckets)
    return [base + (1 if index < extra else 0) for index in range(buckets)]


@dataclass(frozen=True)
class DatasetConfig:
    """数据集参数；默认值与 :class:`IntegrationConfig` 保持一致。"""

    total_rows: int = 1_000_000
    hot_rows: int = 100_000
    days: int = 30
    base_date: str = "2025-09-01"
    seed: int = 20_250_901
    ordinary_customers: int = 9_000
    namespace: str = schema.NAMESPACE
    table: str = schema.TABLE

    @classmethod
    def from_integration_config(cls, config: IntegrationConfig = CONFIG) -> "DatasetConfig":
        return cls(
            total_rows=config.total_rows,
            hot_rows=config.hot_rows,
            days=config.days,
            base_date=config.base_date,
            seed=config.seed,
            ordinary_customers=config.ordinary_customers,
            namespace=config.namespace,
            table=config.table,
        )

    def validate(self) -> None:
        if self.total_rows <= 0:
            raise ValueError("total_rows 必须 > 0")
        if self.hot_rows <= 0:
            raise ValueError("hot_rows 必须 > 0")
        if self.hot_rows > self.total_rows:
            raise ValueError(
                f"hot_rows({self.hot_rows}) 不能大于 total_rows({self.total_rows})"
            )
        if not 1 <= self.days <= 31:
            raise ValueError("days 必须在 1..31 之间")
        if self.hot_rows < self.days:
            raise ValueError(
                f"hot_rows({self.hot_rows}) 必须 >= days({self.days})，否则热客户存在空日"
            )
        if self.ordinary_customers <= 0:
            raise ValueError("ordinary_customers 必须 > 0")
        if self.total_rows > self.hot_rows and self.ordinary_customers <= 0:
            raise ValueError("存在普通客户数据时 ordinary_customers 必须 > 0")
        start = datetime.strptime(self.base_date, "%Y-%m-%d").date()
        end = start + timedelta(days=self.days - 1)
        if (start.year, start.month) != (end.year, end.month):
            raise ValueError(
                f"交易跨度必须落在一个自然月内，当前 {start} ~ {end} 跨月"
            )


@dataclass
class GenerationResult:
    """一次数据生成的结果（供 manifest 与 CLI 输出）。"""

    path: Path
    rows: int
    sha256: str
    bytes_written: int
    elapsed_seconds: float
    config: DatasetConfig = field(default_factory=DatasetConfig)


class Dataset:
    """按配置展开维度并逐行生成数据。"""

    def __init__(self, config: DatasetConfig):
        config.validate()
        self.config = config
        self.base_date = datetime.strptime(config.base_date, "%Y-%m-%d").date()
        self.day_strs = [
            (self.base_date + timedelta(days=index)).strftime("%Y%m%d")
            for index in range(config.days)
        ]
        self.day_iso = [
            (self.base_date + timedelta(days=index)).isoformat()
            for index in range(config.days)
        ]
        self.base_epoch_ms = int(
            datetime(
                self.base_date.year, self.base_date.month, self.base_date.day, tzinfo=BASE_TZ
            ).timestamp()
            * 1000
        )

        self.hot_user_id = "U000001"
        self.ordinary_ids: List[str] = [
            f"U{index:06d}" for index in range(2, 2 + config.ordinary_customers)
        ]
        self._customer_names: Dict[int, str] = {}
        self._merchants: List[Tuple[str, str]] = self._build_merchants()

        ordinary_rows = config.total_rows - config.hot_rows
        self.ordinary_row_counts: List[int] = distribute(ordinary_rows, config.ordinary_customers)
        self.hot_day_counts: List[int] = distribute(config.hot_rows, config.days)
        self._day_counts: Dict[int, List[int]] = {0: self.hot_day_counts}
        for index, count in enumerate(self.ordinary_row_counts, start=1):
            self._day_counts[index] = distribute(count, config.days)
        self._user_index: Dict[str, int] = {self.hot_user_id: 0}
        for index, user_id in enumerate(self.ordinary_ids, start=1):
            self._user_index[user_id] = index

    # -- 维度 ---------------------------------------------------------------

    def _build_merchants(self) -> List[Tuple[str, str]]:
        merchants: List[Tuple[str, str]] = []
        for index in range(200):
            base = MERCHANT_BASE_NAMES[index % len(MERCHANT_BASE_NAMES)]
            name = base if index < len(MERCHANT_BASE_NAMES) else f"{base}（{index // len(MERCHANT_BASE_NAMES)}号店）"
            merchants.append((f"M{index:05d}", name))
        return merchants

    def customer_name(self, user_index: int) -> str:
        name = self._customer_names.get(user_index)
        if name is None:
            surname = SURNAMES[_mix(self.config.seed, user_index, 101) % len(SURNAMES)]
            given = GIVEN_NAMES[_mix(self.config.seed, user_index, 202) % len(GIVEN_NAMES)]
            name = surname + given
            self._customer_names[user_index] = name
        return name

    # -- 单行 ---------------------------------------------------------------

    def _row_values(self, user_index: int, user_id: str, day_index: int, seq: int) -> List[str]:
        config = self.config
        name = self.customer_name(user_index)
        salt = schema.salt_for(user_id)
        day_str = self.day_strs[day_index]
        day_iso = self.day_iso[day_index]

        base = _mix(config.seed, user_index, day_index, seq)
        seconds = base % 86_400
        hour, remainder = divmod(seconds, 3600)
        minute, second = divmod(remainder, 60)
        txn_time = f"{day_iso} {hour:02d}:{minute:02d}:{second:02d}"
        txn_ts = str(self.base_epoch_ms + day_index * 86_400_000 + seconds * 1000)

        direction = DIRECTION_EXPENSE if base % 100 < 85 else DIRECTION_INCOME
        merchant_id, merchant_name = self._merchants[
            _mix(config.seed, user_index, day_index, seq, 2) % len(self._merchants)
        ]
        product_id, product_name, has_logistics = PRODUCTS[
            _mix(config.seed, user_index, day_index, seq, 3) % len(PRODUCTS)
        ]

        currency_roll = _mix(config.seed, user_index, day_index, seq, 4) % 100
        currency = CURRENCIES[-1][0]
        cursor = 0
        for code, weight in CURRENCIES:
            cursor += weight
            if currency_roll < cursor:
                currency = code
                break

        status_roll = _mix(config.seed, user_index, day_index, seq, 5) % 100
        if status_roll < 90:
            status = STATUS_SUCCESS
        elif status_roll < 96:
            status = STATUS_REFUND
        else:
            status = STATUS_PENDING

        amount_cents = 100 + _mix(config.seed, user_index, day_index, seq, 6) % 4_999_900
        amount = f"{amount_cents // 100}.{amount_cents % 100:02d}"

        if has_logistics and status != STATUS_REFUND and direction == DIRECTION_EXPENSE:
            carrier = CARRIERS[_mix(config.seed, user_index, day_index, seq, 7) % len(CARRIERS)]
            number = _mix(config.seed, user_index, day_index, seq, 8) % 1_000_000_000_000
            logistics_no = f"{carrier}{number:012d}"
        else:
            logistics_no = schema.NULL_TOKEN

        remark_roll = _mix(config.seed, user_index, day_index, seq, 9) % 100
        if remark_roll < 80:
            remark = REMARKS[_mix(config.seed, user_index, day_index, seq, 10) % len(REMARKS)]
        elif remark_roll < 92:
            remark = schema.NULL_TOKEN
        else:
            remark = ""

        if direction == DIRECTION_EXPENSE:
            payer_id, payer_name = user_id, name
            payee_id, payee_name = merchant_id, merchant_name
        else:
            payer_id, payer_name = merchant_id, merchant_name
            payee_id, payee_name = user_id, name

        return [
            f"{salt}{user_id}#{day_str}#{seq:06d}",
            payer_id,
            payer_name,
            payee_id,
            payee_name,
            direction,
            txn_time,
            txn_ts,
            day_iso,
            amount,
            currency,
            status,
            product_id,
            product_name,
            logistics_no,
            remark,
        ]

    def day_counts_for(self, user_id: str) -> List[int]:
        """返回某客户每天的交易行数（长度 = days）。"""
        try:
            user_index = self._user_index[user_id]
        except KeyError as exc:
            raise KeyError(f"未知 user_id: {user_id}") from exc
        return list(self._day_counts[user_index])

    def row_for(self, user_id: str, day_index: int, seq: int) -> Dict[str, str]:
        """按 (user_id, day_index, seq) 精确计算一行，供测试断言与 manifest 抽样。"""
        try:
            user_index = self._user_index[user_id]
        except KeyError as exc:
            raise KeyError(f"未知 user_id: {user_id}") from exc
        if not 0 <= day_index < self.config.days:
            raise IndexError(f"day_index 越界: {day_index}")
        count = self._day_counts[user_index][day_index]
        if not 1 <= seq <= count:
            raise IndexError(f"{user_id} 在 day_index={day_index} 只有 {count} 行，seq={seq} 非法")
        values = self._row_values(user_index, user_id, day_index, seq)
        record = {schema.ROWKEY_COLUMN: values[0]}
        for spec, value in zip(schema.COLUMN_SPECS, values[1:]):
            record[spec] = value
        return record

    # -- 遍历 ---------------------------------------------------------------

    def _user_rows(self, user_index: int, user_id: str) -> Iterator[List[str]]:
        day_counts = self._day_counts[user_index]
        for day_index, count in enumerate(day_counts):
            for seq in range(1, count + 1):
                yield self._row_values(user_index, user_id, day_index, seq)

    def iter_rows(self) -> Iterator[List[str]]:
        """逐行产出数据（不含表头）；热客户在前，普通客户按 id 顺序。"""
        yield from self._user_rows(0, self.hot_user_id)
        for user_index, user_id in enumerate(self.ordinary_ids, start=1):
            if self.ordinary_row_counts[user_index - 1] == 0:
                continue
            yield from self._user_rows(user_index, user_id)

    def iter_lines(self) -> Iterator[str]:
        """逐行产出「表头 + 数据」的 TSV 文本（不含换行符）。"""
        yield "\t".join((schema.ROWKEY_COLUMN, *schema.COLUMN_SPECS))
        for values in self.iter_rows():
            yield "\t".join(values)

    @property
    def total_rows(self) -> int:
        return self.config.hot_rows + sum(self.ordinary_row_counts)

    def describe(self) -> Dict[str, object]:
        """数据集的概要信息（供 CLI 打印 / manifest 使用）。"""
        return {
            "total_rows": self.total_rows,
            "hot_rows": self.config.hot_rows,
            "hot_user_id": self.hot_user_id,
            "ordinary_customers": self.config.ordinary_customers,
            "ordinary_rows_min": min(self.ordinary_row_counts) if self.ordinary_row_counts else 0,
            "ordinary_rows_max": max(self.ordinary_row_counts) if self.ordinary_row_counts else 0,
            "days": self.config.days,
            "base_date": self.config.base_date,
            "date_range": [self.day_iso[0], self.day_iso[-1]],
            "hot_day_counts": self.hot_day_counts,
            "hot_prefix": schema.user_prefix(self.hot_user_id),
            "hot_range": list(schema.user_range(self.hot_user_id)),
        }


def generate(
    config: DatasetConfig,
    path: Path,
    *,
    force: bool = False,
    progress: bool = True,
) -> GenerationResult:
    """生成 gzip TSV；返回行数、sha256（未压缩内容）与耗时。"""
    dataset = Dataset(config)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and not force:
        raise FileExistsError(f"数据文件已存在：{path}（使用 force=True 覆盖）")

    hasher = hashlib.sha256()
    rows = 0
    started = time.time()
    tmp_path = path.with_suffix(path.suffix + ".tmp")
    try:
        with gzip.open(tmp_path, "wb", compresslevel=6) as handle:
            for index, line in enumerate(dataset.iter_lines()):
                payload = line.encode("utf-8") + b"\n"
                handle.write(payload)
                hasher.update(payload)
                if index == 0:
                    continue
                rows += 1
                if progress and rows % 200_000 == 0:
                    elapsed = max(time.time() - started, 1e-9)
                    print(
                        f"  生成中 {rows:,}/{config.total_rows:,} 行 "
                        f"({rows / elapsed:,.0f} 行/秒)",
                        flush=True,
                    )
        tmp_path.replace(path)
    finally:
        if tmp_path.exists():
            tmp_path.unlink()

    return GenerationResult(
        path=path,
        rows=rows,
        sha256=hasher.hexdigest(),
        bytes_written=path.stat().st_size,
        elapsed_seconds=time.time() - started,
        config=config,
    )
