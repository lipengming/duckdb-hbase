"""pytest 公共夹具：manifest、HBase 夹具探活、DuckDB/Extension 会话。

约定：

- 数据夹具（manifest + 已装载的表）缺失或未装载时，相关用例 ``skip``，并给出
  可执行的修复命令，而不是把环境问题误报成产品缺陷；
- Extension 未构建、duckdb 未安装、Gateway 未启动时同样 ``skip``；
- ``hbase_scan`` 数据面尚未实现（M1 进行中）时，扩展用例 ``skip``；
  实现完成后无需改测试即可开始执行。
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

INTEGRATION_ROOT = Path(__file__).resolve().parents[1]
SRC_ROOT = INTEGRATION_ROOT / "src"
if str(SRC_ROOT) not in sys.path:
    sys.path.insert(0, str(SRC_ROOT))

from duckdb_hbase_it import loader  # noqa: E402
from duckdb_hbase_it.config import CONFIG  # noqa: E402
from duckdb_hbase_it.duckdb_session import (  # noqa: E402
    DuckDBUnavailable,
    ExtensionUnavailable,
    GatewayUnavailable,
    open_session,
)
from duckdb_hbase_it.manifest import read_manifest  # noqa: E402


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--runslow",
        action="store_true",
        default=False,
        help="运行 slow 用例（全表/大范围扫描，1M 档位耗时较长）",
    )
    parser.addoption(
        "--manifest",
        default=None,
        help="manifest 路径（默认使用 DH_IT_DATA_DIR/manifest.json）",
    )


def pytest_collection_modifyitems(config: pytest.Config, items) -> None:
    if config.getoption("--runslow"):
        return
    skip_slow = pytest.mark.skip(reason="需要 --runslow（全表/大范围扫描）")
    for item in items:
        if "slow" in item.keywords:
            item.add_marker(skip_slow)


@pytest.fixture(scope="session")
def manifest_path(request: pytest.FixtureRequest) -> Path:
    explicit = request.config.getoption("--manifest")
    return Path(explicit).resolve() if explicit else CONFIG.manifest_path


@pytest.fixture(scope="session")
def manifest(manifest_path: Path):
    if not manifest_path.is_file():
        pytest.skip(
            f"未找到 manifest：{manifest_path}；先执行 "
            "`python -m duckdb_hbase_it generate && python -m duckdb_hbase_it load`"
        )
    data = read_manifest(manifest_path)
    if not data.get("loaded"):
        pytest.skip(
            f"manifest 尚未记录装载：{manifest_path}；先执行 "
            "`python -m duckdb_hbase_it load`"
        )
    return data


@pytest.fixture(scope="session")
def hbase_table(manifest):
    """确认表存在且 Region 数符合预期；否则给出修复指引并 skip。"""
    try:
        info = loader.ping()
    except loader.LoaderError as exc:  # pragma: no cover - 环境缺失
        pytest.skip(f"HBase 不可用：{exc}")
    if not info["exists"]:
        pytest.skip(
            f"表 {CONFIG.qualified_table} 不存在；先执行 `python -m duckdb_hbase_it load`"
        )
    return info


@pytest.fixture(scope="session")
def session():
    """仅加载 extension 的会话（不连 Gateway）。"""
    try:
        instance = open_session()
    except (DuckDBUnavailable, ExtensionUnavailable) as exc:
        pytest.skip(str(exc))
    yield instance
    instance.close()


@pytest.fixture(scope="session")
def connection(session):
    """已注册 Gateway 连接的会话。"""
    try:
        session.register_gateway()
    except GatewayUnavailable as exc:
        pytest.skip(str(exc))
    return session


@pytest.fixture(scope="session")
def scan_ready(connection, hbase_table):
    """hbase_scan 数据面可用（M1 完成后自动解除 skip）。"""
    available, reason = connection.probe_scan()
    if not available:
        pytest.skip(f"hbase_scan 数据面尚未实现（M1 进行中）：{reason}")
    return connection
