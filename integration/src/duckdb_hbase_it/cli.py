"""命令行入口：``python -m duckdb_hbase_it <generate|load|verify|status|all>``。

典型用法::

    python -m duckdb_hbase_it generate            # 生成 1,000,000 行 mock 数据
    python -m duckdb_hbase_it load                # 建表（16 预分区）+ 装载
    python -m duckdb_hbase_it verify              # 抽样比对 manifest 与 HBase
    pytest                                        # 数据面 + Extension 用例

单客户三档规模::

    python -m duckdb_hbase_it all --hot-rows 500000   # 10 万 / 50 万 / 100 万
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence

from . import __version__, schema
from .config import CONFIG, IntegrationConfig
from .generator import HOT_TIERS, DatasetConfig, Dataset, generate
from .duckdb_session import (
    DuckDBUnavailable,
    ExtensionUnavailable,
    GatewayUnavailable,
    open_session,
)
from .loader import LoaderError, count, load, ping, scan
from .manifest import (
    build_manifest,
    mark_loaded,
    read_manifest,
    write_manifest,
)


def _dataset_config(args: argparse.Namespace) -> DatasetConfig:
    return DatasetConfig(
        total_rows=getattr(args, "rows", None) or CONFIG.total_rows,
        hot_rows=getattr(args, "hot_rows", None) or CONFIG.hot_rows,
        days=getattr(args, "days", None) or CONFIG.days,
        base_date=getattr(args, "base_date", None) or CONFIG.base_date,
        seed=getattr(args, "seed", None) if getattr(args, "seed", None) is not None else CONFIG.seed,
        ordinary_customers=getattr(args, "ordinary_customers", None) or CONFIG.ordinary_customers,
        namespace=CONFIG.namespace,
        table=CONFIG.table,
    )


def _print_dataset_summary(dataset: Dataset) -> None:
    info = dataset.describe()
    print(
        f"数据集：总行数 {info['total_rows']:,}，热客户 {info['hot_user_id']} "
        f"{info['hot_rows']:,} 行，普通客户 {info['ordinary_customers']:,} 个，"
        f"时间 {info['date_range'][0]} ~ {info['date_range'][1]}（{info['days']} 天）"
    )
    print(
        f"热客户前缀：{info['hot_prefix']}，范围 {info['hot_range']}；"
        f"目前单客户档位：{info['hot_rows']:,}"
    )
    if info["hot_rows"] in HOT_TIERS:
        print(f"档位命中需求定义：{info['hot_rows']:,} 行/客户")
    else:
        print(f"提示：{info['hot_rows']:,} 不是 10 万/50 万/100 万标准档位（可用于 smoke 测试）")


def _require_manifest(config: IntegrationConfig) -> Dict[str, object]:
    if not config.manifest_path.is_file():
        raise SystemExit(
            f"未找到 manifest：{config.manifest_path}\n"
            "请先执行 `python -m duckdb_hbase_it generate` 与 `load`"
        )
    manifest = read_manifest(config.manifest_path)
    table = manifest["table"]
    if table["namespace"] != config.namespace or table["name"] != config.table:
        raise SystemExit(
            "manifest 与当前 HBase 表配置不一致："
            f"manifest={table['namespace']}:{table['name']}，"
            f"当前={config.namespace}:{config.table}\n"
            "请重新 generate 或调整 DH_IT_NAMESPACE / DH_IT_TABLE"
        )
    return manifest


def cmd_generate(args: argparse.Namespace) -> int:
    dataset_config = _dataset_config(args)
    dataset = Dataset(dataset_config)
    _print_dataset_summary(dataset)
    output = Path(args.output) if getattr(args, "output", None) else CONFIG.dataset_tsv_path
    result = generate(
        dataset_config,
        output,
        force=args.force,
        progress=not args.no_progress,
    )
    manifest = build_manifest(dataset, result)
    write_manifest(CONFIG.manifest_path, manifest)
    print(
        f"已生成 {result.rows:,} 行 -> {result.path} "
        f"（{result.bytes_written / 1024 / 1024:.1f} MiB，{result.elapsed_seconds:.1f}s，"
        f"sha256={result.sha256[:16]}...）"
    )
    print(f"manifest 已写入 {CONFIG.manifest_path}")
    return 0


def _verify_samples(manifest: Dict[str, object], sample_limit: int) -> List[str]:
    samples = manifest.get("samples") or []
    if sample_limit > 0:
        samples = samples[:sample_limit]
    if not samples:
        return ["manifest 中没有抽样行，跳过一致性校验"]
    rowkeys = [item["rowkey"] for item in samples]
    fetched = {row["rowkey"]: row for row in scan(rowkeys=rowkeys)}
    problems: List[str] = []
    for sample in samples:
        rowkey = sample["rowkey"]
        row = fetched.get(rowkey)
        if row is None:
            problems.append(f"RowKey 缺失：{rowkey}")
            continue
        for cell, expected in sample["cells"].items():
            actual = row.get(cell)
            if actual != expected:
                problems.append(
                    f"{rowkey} 列 {cell} 不一致：期望 {expected!r}，实际 {actual!r}"
                )
        for cell in sample.get("null_cells", []):
            if row.get(cell) is not None:
                problems.append(f"{rowkey} 列 {cell} 期望 NULL，实际 {row.get(cell)!r}")
    return problems


def cmd_verify(args: argparse.Namespace) -> int:
    manifest = _require_manifest(CONFIG)
    if not manifest.get("loaded"):
        raise SystemExit("manifest 没有装载记录，请先执行 `python -m duckdb_hbase_it load`")

    problems: List[str] = []
    info = ping()
    expected_regions = int(manifest["table"]["salt_buckets"])
    print(f"表 {CONFIG.qualified_table}：exists={info['exists']} regions={info['regions']}")
    if not info["exists"]:
        problems.append("表不存在，请重新 load")
    elif info["regions"] != expected_regions:
        problems.append(f"Region 数 {info['regions']} != 预期 {expected_regions}")

    counts = manifest["counts"]
    hot = counts["hot_range"]
    hot_count = count(start=hot[0], stop=hot[1])
    print(f"热客户范围行数：{hot_count['count']:,}（期望 {counts['hot_rows']:,}）")
    if hot_count["count"] != counts["hot_rows"]:
        problems.append(
            f"热客户行数不一致：{hot_count['count']} != {counts['hot_rows']}"
        )

    if args.full_count:
        total = count()
        print(f"全表行数：{total['count']:,}（期望 {counts['total_rows']:,}）")
        if total["count"] != counts["total_rows"]:
            problems.append(f"全表行数不一致：{total['count']} != {counts['total_rows']}")

    problems.extend(_verify_samples(manifest, args.samples))
    print(f"抽样一致性：{'通过' if not problems else '发现 %d 个问题' % len(problems)}")
    for problem in problems:
        print(f"  - {problem}")
    if problems:
        return 1
    print("verify 通过")
    return 0


def cmd_load(args: argparse.Namespace) -> int:
    manifest = _require_manifest(CONFIG)
    dataset = manifest["dataset"]
    if args.hot_rows and int(args.hot_rows) != dataset["hot_rows"]:
        raise SystemExit(
            f"--hot-rows={args.hot_rows} 与 manifest 记录的 {dataset['hot_rows']} 不一致；"
            "请先重新 generate"
        )
    print(
        f"装载 {CONFIG.qualified_table}：manifest 记录 {dataset['total_rows']:,} 行，"
        f"热客户 {dataset['hot_rows']:,} 行"
    )
    result = load(
        input_path=Path(manifest["file"]["path"]),
        recreate=not args.append,
        append=args.append,
        progress_every=args.progress_every,
        stream_progress=not args.no_progress,
    )
    if int(result["rows"]) != int(dataset["total_rows"]):
        raise SystemExit(
            f"装载行数 {result['rows']} != manifest 期望 {dataset['total_rows']}"
        )
    mark_loaded(
        CONFIG.manifest_path,
        {
            "rows": result["rows"],
            "cells": result["cells"],
            "elapsed_ms": result["ms"],
            "table": CONFIG.qualified_table,
        },
    )
    print(
        f"装载完成：{result['rows']:,} 行，{result['cells']:,} Cell，"
        f"{result['ms'] / 1000:.1f}s"
    )
    return 0


def cmd_status(args: argparse.Namespace) -> int:
    print(f"集成测试包版本：{__version__}")
    print(f"表：{CONFIG.qualified_table}（{len(schema.FAMILIES)} 个列族，"
          f"{len(schema.COLUMN_SPECS)} 列，{schema.SALT_BUCKETS} 个 salt 桶）")
    print(f"manifest：{CONFIG.manifest_path}"
          f"{'（存在）' if CONFIG.manifest_path.is_file() else '（不存在）'}")
    if CONFIG.manifest_path.is_file():
        manifest = read_manifest(CONFIG.manifest_path)
        dataset = manifest["dataset"]
        print(
            f"  seed={dataset['seed']} 总行数={dataset['total_rows']:,} "
            f"热客户档位={dataset['hot_rows']:,} 天数={dataset['days']} "
            f"起始日={dataset['base_date']}"
        )
        loaded = manifest.get("loaded")
        print(f"  装载：{loaded if loaded else '尚未装载'}")
    try:
        info = ping()
        print(f"HBase 探活：{json.dumps(info, ensure_ascii=False)}")
    except LoaderError as exc:
        print(f"HBase 探活失败：{exc}")
        return 2
    return 0


def cmd_bootstrap(args: argparse.Namespace) -> int:
    """初始化 DuckDB + hbase extension，并按需注册 Gateway 连接。

    这是「测试脚本初始化 duckdb + hbase extension」的最小入口：
    不依赖 pytest，任何环境都能用一条命令确认扩展加载、连接注册与 scan 能力。
    """
    try:
        session = open_session()
    except (DuckDBUnavailable, ExtensionUnavailable) as exc:
        print(f"初始化失败：{exc}", file=sys.stderr)
        return 2
    try:
        print(f"DuckDB + extension 初始化成功：hbase_version={session.version}")
        print(f"extension 路径：{session.extension_path}")
        if not args.connect:
            return 0
        try:
            session.register_gateway()
        except GatewayUnavailable as exc:
            print(f"Gateway 注册失败：{exc}", file=sys.stderr)
            return 2
        print(f"Gateway 连接已注册：{session.gateway}")
        available, reason = session.probe_scan()
        if available:
            print("hbase_scan 数据面可用")
        else:
            print(f"hbase_scan 数据面暂不可用（M1）：{reason}")
        return 0
    finally:
        session.close()


def cmd_all(args: argparse.Namespace) -> int:
    if not CONFIG.manifest_path.is_file() or args.force:
        code = cmd_generate(args)
        if code != 0:
            return code
    else:
        print("已存在 manifest，跳过生成（--force 可重新生成）")
    code = cmd_load(args)
    if code != 0:
        return code
    return cmd_verify(args)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m duckdb_hbase_it",
        description="DuckDB-HBase 客户交易宽表集成测试数据准备工具",
    )
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    subparsers = parser.add_subparsers(dest="command", required=True)

    def add_dataset_options(sub: argparse.ArgumentParser) -> None:
        sub.add_argument("--rows", type=int, default=None, help="数据集总行数（默认 1,000,000）")
        sub.add_argument(
            "--hot-rows",
            type=int,
            default=None,
            help="单客户（热客户）行数，标准档位 100000/500000/1000000（默认 100000）",
        )
        sub.add_argument("--days", type=int, default=None, help="交易时间跨度天数（默认 30，上限 31）")
        sub.add_argument("--base-date", default=None, help="起始日期 YYYY-MM-DD（默认 2025-09-01）")
        sub.add_argument("--seed", type=int, default=None, help="随机种子（默认 20250901）")
        sub.add_argument(
            "--ordinary-customers", type=int, default=None, help="普通客户数（默认 9000）"
        )
        sub.add_argument("--no-progress", action="store_true", help="关闭进度输出")

    generate_parser = subparsers.add_parser("generate", help="生成 mock 数据与 manifest")
    add_dataset_options(generate_parser)
    generate_parser.add_argument("--output", default=None, help="输出 TSV.gz 路径")
    generate_parser.add_argument("--force", action="store_true", help="覆盖已有数据文件")
    generate_parser.set_defaults(func=cmd_generate)

    load_parser = subparsers.add_parser("load", help="建表并批量装载到 HBase")
    load_parser.add_argument("--append", action="store_true", help="追加到已有表（不重建）")
    load_parser.add_argument("--hot-rows", type=int, default=None, help="校验 manifest 档位")
    load_parser.add_argument("--progress-every", type=int, default=100_000, help="进度打印间隔")
    load_parser.add_argument("--no-progress", action="store_true", help="关闭装载进度")
    load_parser.set_defaults(func=cmd_load)

    verify_parser = subparsers.add_parser("verify", help="校验 Region/行数/抽样单元格")
    verify_parser.add_argument("--full-count", action="store_true", help="额外做全表计数（较慢）")
    verify_parser.add_argument("--samples", type=int, default=0, help="抽样条数上限（0=全部）")
    verify_parser.set_defaults(func=cmd_verify)

    status_parser = subparsers.add_parser("status", help="打印配置、manifest 与 HBase 探活")
    status_parser.set_defaults(func=cmd_status)

    bootstrap_parser = subparsers.add_parser(
        "bootstrap", help="初始化 DuckDB + hbase extension（可选注册 Gateway）"
    )
    bootstrap_parser.add_argument(
        "--connect", action="store_true", help="注册 Gateway 连接并探测 scan 能力"
    )
    bootstrap_parser.set_defaults(func=cmd_bootstrap)

    all_parser = subparsers.add_parser("all", help="generate + load + verify")
    add_dataset_options(all_parser)
    all_parser.add_argument("--force", action="store_true", help="重新生成数据")
    all_parser.add_argument("--append", action="store_true", help="追加装载，不重建表")
    all_parser.add_argument("--progress-every", type=int, default=100_000)
    all_parser.add_argument("--full-count", action="store_true", help="verify 时做全表计数")
    all_parser.add_argument("--samples", type=int, default=0, help="verify 抽样条数上限")
    all_parser.set_defaults(func=cmd_all)

    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.func(args))
    except (LoaderError, SystemExit) as exc:
        if isinstance(exc, SystemExit):
            code = exc.code if isinstance(exc.code, int) else 2
            if exc.code:
                print(str(exc.code), file=sys.stderr)
            return code
        print(f"错误：{exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:  # pragma: no cover
        print("已中断", file=sys.stderr)
        return 130


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
