"""DuckDB + hbase.duckdb_extension 的初始化与会话封装。

注意事项：

1. 仓库根目录下的 ``duckdb/`` 是 DuckDB 源码 submodule，Python 3 会把它当成
   namespace package 导入，导致 ``import duckdb`` 拿到目录而不是 pip 包。
   本模块在导入前临时把仓库根目录从 ``sys.path`` 移除，并校验 ``__version__``。
2. 本地 extension 未签名，Python 连接必须显式打开 ``allow_unsigned_extensions``。
3. ``hbase_connect`` 建连即做 Health 检查，Gateway 未启动时会抛错；本模块
   把它映射为 :class:`GatewayUnavailable`，由测试 skip 而不是误报失败。
4. 当前 M1 阶段 ``hbase_scan`` 数据面（Arrow IPC）尚未实现，探测到
   NotImplemented 时以 :class:`ScanNotImplemented` 标记，测试据此 skip；
   实现完成后测试自动开始执行，不需要改测试代码。
"""

from __future__ import annotations

import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, List, Optional, Sequence, Set, Tuple

from .config import CONFIG, REPO_ROOT, IntegrationConfig


class DuckDBUnavailable(RuntimeError):
    """duckdb Python 包不可用。"""


class ExtensionUnavailable(RuntimeError):
    """hbase.duckdb_extension 构建产物不存在或加载失败。"""


class GatewayUnavailable(RuntimeError):
    """Gateway 不可达 / 连接注册失败。"""


class ScanNotImplemented(RuntimeError):
    """hbase_scan 数据面尚未实现（M1 进行中）。"""


def import_duckdb():
    """干净地导入 pip 安装的 duckdb（绕开仓库 duckdb/ 源码目录）。"""
    bad = sys.modules.get("duckdb")
    if bad is not None and not hasattr(bad, "__version__"):
        del sys.modules["duckdb"]

    saved = sys.path[:]
    try:
        sys.path[:] = [
            entry
            for entry in sys.path
            if Path(entry or ".").resolve() != REPO_ROOT
        ]
        import importlib

        try:
            module = importlib.import_module("duckdb")
        except ImportError as exc:
            raise DuckDBUnavailable(
                "未安装 duckdb Python 包（需与 extension 构建版本一致，例如 duckdb==2.0.0）："
                "`pip install 'duckdb==2.0.0'`"
            ) from exc
    finally:
        sys.path[:] = saved

    if not hasattr(module, "__version__") or not hasattr(module, "connect"):
        raise DuckDBUnavailable(
            "import duckdb 命中了仓库下的 duckdb/ 源码目录而不是 Python 包；"
            "请在仓库外的工作目录运行，或安装 pip duckdb 包"
        )
    return module


def sql_literal(value: str) -> str:
    """把字符串转成 SQL 单引号字面量（转义单引号）。"""
    return "'" + value.replace("'", "''") + "'"


def quote_identifier(name: str) -> str:
    """列名（如 ``party:payer_id``）在 SQL 中需要双引号。"""
    return '"' + name.replace('"', '""') + '"'


def _is_missing_function(message: str) -> bool:
    lowered = message.lower()
    return (
        "does not exist" in lowered
        or "no function matches" in lowered
        or "not found" in lowered
        or "不存在" in message
    )


def _is_not_implemented(message: str) -> bool:
    lowered = message.lower()
    return (
        "尚未实现" in message
        or "not implemented" in lowered
        or "unimplemented" in lowered
        or "not_implemented" in lowered
    )


@dataclass
class IntegrationSession:
    """一个 DuckDB 连接及其能力探测结果。"""

    con: Any
    version: str
    extension_path: Path
    gateway: str
    connection: str
    scan_available: bool = False
    scan_reason: str = ""
    supported_functions: Set[str] = field(default_factory=set)
    gateway_connected: bool = False

    # -- 基础查询 -------------------------------------------------------

    def execute(self, sql: str) -> Any:
        return self.con.execute(sql)

    def fetchall(self, sql: str) -> List[Tuple[Any, ...]]:
        return self.con.execute(sql).fetchall()

    def fetchone(self, sql: str) -> Optional[Tuple[Any, ...]]:
        return self.con.execute(sql).fetchone()

    def scalar(self, sql: str) -> Any:
        row = self.fetchone(sql)
        return None if row is None else row[0]

    # -- 能力 -----------------------------------------------------------

    def register_gateway(self) -> None:
        """执行 hbase_connect；失败抛 GatewayUnavailable。"""
        sql = (
            "SELECT hbase_connect("
            f"{sql_literal(self.connection)}, {sql_literal(self.gateway)})"
        )
        try:
            result = self.con.execute(sql).fetchone()
        except Exception as exc:  # duckdb.Error 的类型随版本不同，统一兜住
            raise GatewayUnavailable(
                f"Gateway 连接失败（{self.gateway}）：{exc}"
            ) from exc
        if not result or not result[0]:
            raise GatewayUnavailable(f"hbase_connect 返回 false（{self.gateway}）")
        self.gateway_connected = True

    def probe_scan(self) -> Tuple[bool, str]:
        """用空范围探测 hbase_scan 数据面是否可用，不读取真实数据。"""
        columns = ["party:payer_id"]
        sql = (
            "SELECT count(*) FROM hbase_scan("
            f"{sql_literal(self.connection)}, "
            f"{sql_literal(CONFIG.namespace)}, "
            f"{sql_literal(CONFIG.table)}, "
            "start_row := '~', stop_row := '~~', "
            f"columns := [{', '.join(sql_literal(item) for item in columns)}])"
        )
        try:
            self.con.execute(sql).fetchone()
        except Exception as exc:
            message = str(exc)
            if _is_not_implemented(message):
                self.scan_available = False
                self.scan_reason = message.strip()
                return False, self.scan_reason
            raise
        self.scan_available = True
        return True, ""

    def probe_function(self, name: str, probe_sql: str) -> bool:
        """探测 SQL 函数是否存在（不存在返回 False，其余异常原样抛出）。"""
        if name in self.supported_functions:
            return True
        try:
            self.con.execute(probe_sql).fetchall()
        except Exception as exc:
            if _is_missing_function(str(exc)):
                return False
            raise
        self.supported_functions.add(name)
        return True

    def has_cache_stats(self) -> bool:
        return self.probe_function(
            "hbase_cache_stats", "SELECT * FROM hbase_cache_stats() LIMIT 0"
        )

    def has_cache_clear(self) -> bool:
        return self.probe_function("hbase_cache_clear", "SELECT hbase_cache_clear()")

    def has_get(self) -> bool:
        return self.probe_function(
            "hbase_get",
            "SELECT * FROM hbase_get("
            f"{sql_literal(self.connection)}, {sql_literal(CONFIG.namespace)}, "
            f"{sql_literal(CONFIG.table)}, rowkeys := ['__probe__']) LIMIT 0",
        )

    def close(self) -> None:
        try:
            self.con.close()
        except Exception:
            pass


def find_extension(config: IntegrationConfig = CONFIG) -> Path:
    extension = config.resolve_extension()
    if extension is None:
        searched = "\n".join(f"  - {path}" for path in config.extension_candidates)
        raise ExtensionUnavailable(
            "未找到 hbase.duckdb_extension 构建产物，已查找：\n"
            f"{searched}\n"
            "请先构建 extension（GEN=ninja EXT_FLAGS=\"-DCMAKE_POLICY_VERSION_MINIMUM=3.5\" make release），"
            "或通过 DH_IT_EXTENSION 指定路径"
        )
    return extension


def open_session(
    config: IntegrationConfig = CONFIG,
    *,
    database: Optional[str] = None,
    register_gateway: bool = False,
) -> IntegrationSession:
    """加载扩展并返回会话；Gateway 连接默认由调用方显式触发。"""
    duckdb = import_duckdb()
    extension = find_extension(config)
    try:
        con = duckdb.connect(
            database if database is not None else config.duckdb_database or ":memory:",
            config={"allow_unsigned_extensions": "true"},
        )
        con.execute(f"LOAD {sql_literal(str(extension))}")
        version = str(con.execute("SELECT hbase_version()").fetchone()[0])
    except Exception as exc:
        raise ExtensionUnavailable(f"加载 extension 失败（{extension}）：{exc}") from exc

    session = IntegrationSession(
        con=con,
        version=version,
        extension_path=extension,
        gateway=config.gateway,
        connection=config.connection,
    )
    if register_gateway:
        session.register_gateway()
    return session


def table_scan_sql(
    *,
    connection: str,
    namespace: str,
    table: str,
    columns: Sequence[str],
    start_row: Optional[str] = None,
    stop_row: Optional[str] = None,
    cache: Optional[bool] = None,
) -> str:
    """拼装 hbase_scan(...) 调用（显式 columns，避免依赖 GetSchema 的类型推断）。"""
    args = [
        sql_literal(connection),
        sql_literal(namespace),
        sql_literal(table),
    ]
    if start_row is not None:
        args.append(f"start_row := {sql_literal(start_row)}")
    if stop_row is not None:
        args.append(f"stop_row := {sql_literal(stop_row)}")
    args.append(f"columns := [{', '.join(sql_literal(item) for item in columns)}]")
    if cache is not None:
        args.append(f"cache := {'true' if cache else 'false'}")
    return "hbase_scan(" + ", ".join(args) + ")"
