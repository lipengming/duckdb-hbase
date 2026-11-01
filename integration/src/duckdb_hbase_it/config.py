"""集成测试包的配置解析（全部支持环境变量覆盖，便于 CI 与多环境）。

约定的环境变量（均以 ``DH_IT_`` 开头）：

=====================  ==========================================  ==========================
变量                    默认值                                        说明
=====================  ==========================================  ==========================
DH_IT_NAMESPACE         duckdb_test                                 HBase 命名空间
DH_IT_TABLE             customer_transaction                        客户交易宽表名
DH_IT_TOTAL_ROWS        1000000                                     数据集总行数
DH_IT_HOT_ROWS          100000                                      单客户（热客户）行数档位
DH_IT_DAYS              30                                          交易时间跨度（天，<=31）
DH_IT_BASE_DATE         2025-09-01                                  起始日期（含）
DH_IT_SEED              20250901                                    mock 数据随机种子
DH_IT_ORDINARY_CUSTOMERS 9000                                       普通客户数（瓜分剩余行数）
DH_IT_DATA_DIR          integration/.cache/datasets                 数据与 manifest 目录
DH_IT_GATEWAY           grpc://127.0.0.1:18080                      Gateway gRPC 端点
DH_IT_CONNECTION        it                                          Extension 连接名
DH_IT_EXTENSION         (自动探测 build/{release,debug}/... )      hbase.duckdb_extension 路径
DH_IT_DUCKDB            integration/.cache/it.duckdb               DuckDB 数据库文件（空串=内存库）
DH_IT_JAVA              java                                        Java 可执行文件
DH_IT_HBASE             hbase                                       hbase 可执行文件
=====================  ==========================================  ==========================
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional

#: 仓库根目录（integration/src/duckdb_hbase_it/config.py -> 向上 4 层）。
REPO_ROOT = Path(__file__).resolve().parents[3]

#: 集成测试包目录（integration/）。
PACKAGE_ROOT = REPO_ROOT / "integration"


def _env_str(name: str, default: str) -> str:
    value = os.environ.get(name)
    return value if value not in (None, "") else default


def _env_int(name: str, default: int) -> int:
    raw = os.environ.get(name)
    if raw in (None, ""):
        return default
    try:
        return int(raw)
    except ValueError as exc:
        raise ValueError(f"环境变量 {name} 必须是整数，当前为 {raw!r}") from exc


def _env_path(name: str, default: Path) -> Path:
    raw = os.environ.get(name)
    return Path(raw).expanduser().resolve() if raw else default


@dataclass
class IntegrationConfig:
    """集成测试的运行时配置（不可变约定：构造后不再修改）。"""

    # ---- HBase ----
    namespace: str = "duckdb_test"
    table: str = "customer_transaction"
    hbase_bin: str = "hbase"
    java_bin: str = "java"
    javac_bin: str = "javac"

    # ---- 数据集 ----
    total_rows: int = 1_000_000
    hot_rows: int = 100_000
    days: int = 30
    base_date: str = "2025-09-01"
    seed: int = 20_250_901
    ordinary_customers: int = 9_000

    # ---- 目录 ----
    data_dir: Path = field(default_factory=lambda: PACKAGE_ROOT / ".cache" / "datasets")
    classes_dir: Path = field(default_factory=lambda: PACKAGE_ROOT / ".cache" / "loader-classes")

    # ---- Extension / Gateway ----
    gateway: str = "grpc://127.0.0.1:18080"
    connection: str = "it"
    duckdb_database: str = ""
    extension_candidates: List[Path] = field(default_factory=list)

    @classmethod
    def from_env(cls) -> "IntegrationConfig":
        data_dir = _env_path("DH_IT_DATA_DIR", PACKAGE_ROOT / ".cache" / "datasets")
        classes_dir = PACKAGE_ROOT / ".cache" / "loader-classes"
        default_exts = [
            REPO_ROOT / "build" / "release" / "extension" / "hbase" / "hbase.duckdb_extension",
            REPO_ROOT / "build" / "debug" / "extension" / "hbase" / "hbase.duckdb_extension",
            REPO_ROOT / "build" / "relassert" / "extension" / "hbase" / "hbase.duckdb_extension",
        ]
        explicit = os.environ.get("DH_IT_EXTENSION")
        candidates = ([Path(explicit).expanduser().resolve()] if explicit else []) + default_exts
        return cls(
            namespace=_env_str("DH_IT_NAMESPACE", "duckdb_test"),
            table=_env_str("DH_IT_TABLE", "customer_transaction"),
            hbase_bin=_env_str("DH_IT_HBASE", "hbase"),
            java_bin=_env_str("DH_IT_JAVA", "java"),
            javac_bin=_env_str("DH_IT_JAVAC", "javac"),
            total_rows=_env_int("DH_IT_TOTAL_ROWS", 1_000_000),
            hot_rows=_env_int("DH_IT_HOT_ROWS", 100_000),
            days=_env_int("DH_IT_DAYS", 30),
            base_date=_env_str("DH_IT_BASE_DATE", "2025-09-01"),
            seed=_env_int("DH_IT_SEED", 20_250_901),
            ordinary_customers=_env_int("DH_IT_ORDINARY_CUSTOMERS", 9_000),
            data_dir=data_dir,
            classes_dir=classes_dir,
            gateway=_env_str("DH_IT_GATEWAY", "grpc://127.0.0.1:18080"),
            connection=_env_str("DH_IT_CONNECTION", "it"),
            duckdb_database=_env_str("DH_IT_DUCKDB", ""),
            extension_candidates=candidates,
        )

    # ---- 派生路径 ----

    @property
    def manifest_path(self) -> Path:
        return self.data_dir / "manifest.json"

    @property
    def dataset_tsv_path(self) -> Path:
        return self.data_dir / "customer_transaction.tsv.gz"

    @property
    def loader_source(self) -> Path:
        return Path(__file__).resolve().parent / "java" / "HBaseBulkLoader.java"

    @property
    def loader_class(self) -> str:
        return "HBaseBulkLoader"

    @property
    def qualified_table(self) -> str:
        return f"{self.namespace}:{self.table}"

    def resolve_extension(self) -> Optional[Path]:
        """返回第一个存在的 extension 路径；都不存在时返回 None。"""
        for candidate in self.extension_candidates:
            if candidate.is_file():
                return candidate
        return None


CONFIG = IntegrationConfig.from_env()
