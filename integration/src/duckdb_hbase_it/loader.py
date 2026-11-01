"""HBase 夹具操作封装：编译/调用 Java 批量装载器，并提供 ping/count/scan/load。

选择 Java + BufferedMutator 的原因见 ``java/HBaseBulkLoader.java`` 注释：1M 行 × 15 列
用 hbase shell 逐行 put 不现实。本模块只依赖 JDK 11 与 HBase 自带客户端 jar
（``hbase classpath``），不引入 Python 侧 HBase 客户端依赖。

实现细节：

- 编译产物缓存在 ``integration/.cache/loader-classes``，源码更新会自动重编译；
- 本机 macOS 上 ``InetAddress.getLocalHost()`` 反查可能耗时 5~6 秒（HBase 客户端
  建连固定开销），因此当 HBase ZooKeeper quorum 指向本机且反查确实慢时，
  自动生成仅含 localhost/本机 hostname 的 hosts 文件并通过
  ``-Djdk.net.hosts.file`` 传给 JVM；远端集群不会启用该 workaround；
- 空 Cell 在 scan 输出中用 ``\\N`` 表示，解析为 ``None``；显式空串保留为 ``""``。
"""

from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Dict, List, Optional, Sequence

from . import schema
from .config import CONFIG, PACKAGE_ROOT, IntegrationConfig

DEFAULT_TIMEOUT = 300
LOAD_TIMEOUT = 3600


class LoaderError(RuntimeError):
    """装载器/Java 进程失败。"""


_classpath_cache: Dict[str, str] = {}
_dns_workaround_cache: Optional[str] = None


def hbase_classpath(config: IntegrationConfig = CONFIG) -> str:
    """返回 ``hbase classpath`` 的 classpath（含 hbase-site.xml 所在目录）。"""
    cached = _classpath_cache.get(config.hbase_bin)
    if cached:
        return cached
    executable = shutil.which(config.hbase_bin)
    if executable is None:
        raise LoaderError(
            f"未找到 {config.hbase_bin!r} 命令；请先按 docs/environment.md 安装 HBase 2.6.x"
        )
    completed = subprocess.run(
        [executable, "classpath"],
        capture_output=True,
        text=True,
        timeout=120,
    )
    if completed.returncode != 0:
        raise LoaderError(
            "`hbase classpath` 执行失败：\n" + (completed.stderr or "").strip()[-2000:]
        )
    lines = [line.strip() for line in completed.stdout.splitlines() if line.strip()]
    if not lines:
        raise LoaderError("`hbase classpath` 没有返回 classpath")
    _classpath_cache[config.hbase_bin] = lines[-1]
    return lines[-1]


def ensure_loader(config: IntegrationConfig = CONFIG) -> Path:
    """编译 Java 装载器（必要时），返回 classes 目录。"""
    classes = config.classes_dir
    class_file = classes / f"{config.loader_class}.class"
    source = config.loader_source
    if class_file.is_file() and class_file.stat().st_mtime >= source.stat().st_mtime:
        return classes

    classes.mkdir(parents=True, exist_ok=True)
    try:
        completed = subprocess.run(
            [
                config.javac_bin,
                "-encoding",
                "UTF-8",
                "-cp",
                hbase_classpath(config),
                "-d",
                str(classes),
                str(source),
            ],
            capture_output=True,
            text=True,
            timeout=300,
        )
    except FileNotFoundError as exc:
        raise LoaderError(f"未找到 javac（{config.javac_bin}），请安装 JDK 11") from exc
    except subprocess.TimeoutExpired as exc:
        raise LoaderError("编译 HBaseBulkLoader.java 超时（300s）") from exc
    if completed.returncode != 0:
        raise LoaderError(
            "编译 HBaseBulkLoader.java 失败：\n" + (completed.stderr or "").strip()[-4000:]
        )
    return classes


def _hbase_quorum(config: IntegrationConfig) -> Optional[str]:
    """从 ``hbase classpath`` 的首个目录读取 hbase-site.xml 的 ZooKeeper quorum。"""
    first = hbase_classpath(config).split(os.pathsep)[0]
    site = Path(first) / "hbase-site.xml"
    if not site.is_file():
        return None
    try:
        root = ET.parse(site).getroot()
    except ET.ParseError:
        return None
    for prop in root.findall("property"):
        name = prop.findtext("name")
        if name == "hbase.zookeeper.quorum":
            return (prop.findtext("value") or "").strip()
    return None


def _hosts_file(config: IntegrationConfig) -> Optional[str]:
    """返回 ``-Djdk.net.hosts.file`` 的路径；远端集群返回 None。

    macOS 上 HBase 客户端建连时 ``InetAddress.getLocalHost()`` 的反向解析
    可能卡 5~6 秒（mDNS 超时）。HBase 客户端在 ZooKeeper quorum 指向本机时
    不需要解析任何外部主机名，因此可以安全地把本机 hostname 固定到
    127.0.0.1；quorum 不是本机时（远端集群）不启用，避免破坏外部域名解析。
    """
    global _dns_workaround_cache
    explicit = os.environ.get("DH_IT_HOSTS_FILE")
    if explicit:
        return explicit
    if _dns_workaround_cache is not None:
        return _dns_workaround_cache or None

    _dns_workaround_cache = ""
    quorum = _hbase_quorum(config) or "localhost"
    hosts = [host.strip() for host in quorum.split(",") if host.strip()]
    if not all(host in ("localhost", "127.0.0.1", "::1") for host in hosts):
        return None

    hostname = socket.gethostname()
    path = PACKAGE_ROOT / ".cache" / "hosts"
    path.parent.mkdir(parents=True, exist_ok=True)
    content = (
        "127.0.0.1 localhost\n"
        f"127.0.0.1 {hostname}\n"
        "::1 localhost\n"
    )
    if not path.is_file() or path.read_text(encoding="utf-8") != content:
        path.write_text(content, encoding="utf-8")
    _dns_workaround_cache = str(path)
    return str(path)


def _base_command(config: IntegrationConfig, args: Sequence[str]) -> List[str]:
    classes = ensure_loader(config)
    classpath = os.pathsep.join([hbase_classpath(config), str(classes)])
    command = [config.java_bin, "-Xmx1g"]
    hosts = _hosts_file(config)
    if hosts:
        command.append(f"-Djdk.net.hosts.file={hosts}")
    command.extend(["-cp", classpath, config.loader_class, *args])
    return command


def _invoke(
    config: IntegrationConfig,
    args: Sequence[str],
    *,
    timeout: int = DEFAULT_TIMEOUT,
    stream_stderr: bool = False,
) -> str:
    command = _base_command(config, args)
    process = None
    try:
        if stream_stderr:
            process = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            assert process.stderr is not None
            stdout_parts: List[str] = []
            import threading

            def _pump_stdout() -> None:
                assert process.stdout is not None
                stdout_parts.append(process.stdout.read())

            reader = threading.Thread(target=_pump_stdout, daemon=True)
            reader.start()
            for line in process.stderr:
                print(line.rstrip("\n"), file=sys.stderr)
            returncode = process.wait(timeout=timeout)
            reader.join(timeout=10)
            stdout = "".join(stdout_parts)
            stderr = ""
        else:
            completed = subprocess.run(
                command, capture_output=True, text=True, timeout=timeout
            )
            returncode, stdout, stderr = completed.returncode, completed.stdout, completed.stderr
    except FileNotFoundError as exc:
        raise LoaderError(
            f"未找到 Java（{config.java_bin}），请安装 JDK 11"
        ) from exc
    except subprocess.TimeoutExpired as exc:
        if process is not None:
            process.kill()
        raise LoaderError(f"Java 装载器执行超时（{timeout}s）：{' '.join(args)}") from exc

    if returncode != 0:
        detail = (stderr or "").strip()
        if not detail:
            detail = (stdout or "").strip()
        raise LoaderError(
            "Java 装载器执行失败（exit="
            f"{returncode}）：{' '.join(args)}\n{detail[-4000:]}"
        )
    return stdout


def _last_json(stdout: str) -> Dict[str, object]:
    for line in reversed([item.strip() for item in stdout.splitlines() if item.strip()]):
        if line.startswith("{"):
            try:
                return json.loads(line)
            except json.JSONDecodeError as exc:  # pragma: no cover - 防御性
                raise LoaderError(f"无法解析装载器输出：{line}") from exc
    raise LoaderError("装载器没有返回 JSON 结果")


def ping(config: IntegrationConfig = CONFIG) -> Dict[str, object]:
    """检查表存在性与 Region 数。"""
    return _last_json(
        _invoke(
            config,
            ["--mode=ping", f"--table={config.qualified_table}"],
            timeout=120,
        )
    )


def count(
    config: IntegrationConfig = CONFIG,
    *,
    start: Optional[str] = None,
    stop: Optional[str] = None,
) -> Dict[str, object]:
    """按 RowKey 范围计数（左闭右开）。"""
    args = ["--mode=count", f"--table={config.qualified_table}"]
    if start:
        args.append(f"--start={start}")
    if stop:
        args.append(f"--stop={stop}")
    return _last_json(_invoke(config, args))


def scan(
    config: IntegrationConfig = CONFIG,
    *,
    start: Optional[str] = None,
    stop: Optional[str] = None,
    limit: int = 100,
    columns: Sequence[str] = schema.COLUMN_SPECS,
    rowkeys: Optional[Sequence[str]] = None,
    timeout: int = DEFAULT_TIMEOUT,
) -> List[Dict[str, Optional[str]]]:
    """按范围或指定 rowkeys 读取行；返回 ``[{rowkey, "family:qualifier": value|None}]``。"""
    if not columns:
        raise ValueError("columns 不能为空")
    args = [
        "--mode=scan",
        f"--table={config.qualified_table}",
        f"--columns={','.join(columns)}",
        f"--limit={limit}",
    ]
    if rowkeys is not None:
        if not rowkeys:
            return []
        args.append(f"--rowkeys={','.join(rowkeys)}")
    else:
        if start:
            args.append(f"--start={start}")
        if stop:
            args.append(f"--stop={stop}")

    stdout = _invoke(config, args, timeout=timeout)
    lines = [line for line in stdout.splitlines() if line != ""]
    if not lines:
        return []
    header = lines[0].split("\t")
    rows: List[Dict[str, Optional[str]]] = []
    for line in lines[1:]:
        fields = line.split("\t")
        if len(fields) != len(header):
            raise LoaderError(f"scan 输出列数不一致：期望 {len(header)}，实际 {len(fields)}")
        record: Dict[str, Optional[str]] = {"rowkey": fields[0]}
        for name, value in zip(header[1:], fields[1:]):
            record[name] = None if value == schema.NULL_TOKEN else value
        rows.append(record)
    return rows


def load(
    config: IntegrationConfig = CONFIG,
    *,
    input_path: Optional[Path] = None,
    recreate: bool = True,
    append: bool = False,
    families: Sequence[str] = schema.FAMILIES,
    splits: Optional[Sequence[str]] = None,
    progress_every: int = 100_000,
    stream_progress: bool = True,
) -> Dict[str, object]:
    """建表（可预分区）并批量装载 gzip TSV。"""
    source = Path(input_path) if input_path else config.dataset_tsv_path
    if not source.is_file():
        raise LoaderError(
            f"数据文件不存在：{source}；请先执行 `python -m duckdb_hbase_it generate`"
        )
    if recreate and append:
        raise ValueError("recreate 与 append 不能同时为 True")
    args = [
        "--mode=load",
        f"--table={config.qualified_table}",
        f"--input={source}",
        f"--families={','.join(families)}",
        f"--progress-every={progress_every}",
    ]
    if splits is None:
        splits = schema.split_keys()
    if splits:
        args.append(f"--splits={','.join(splits)}")
    if recreate:
        args.append("--recreate")
    elif append:
        args.append("--append")
    stdout = _invoke(config, args, timeout=LOAD_TIMEOUT, stream_stderr=stream_progress)
    return _last_json(stdout)
