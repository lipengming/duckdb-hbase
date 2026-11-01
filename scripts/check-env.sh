#!/usr/bin/env bash
# 检查 DuckDB-HBase 开发/测试环境是否就绪。
# 用法：bash scripts/check-env.sh [--strict]
#   --strict  警告也视为失败（CI 用）
set -uo pipefail

STRICT=0
[ "${1:-}" = "--strict" ] && STRICT=1

PASS=0
WARN=0
FAIL=0

ok()   { PASS=$((PASS + 1)); printf '  \033[32mPASS\033[0m  %s\n' "$1"; }
warn() { WARN=$((WARN + 1)); printf '  \033[33mWARN\033[0m  %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m  %s\n' "$1"; }
head_() { printf '\n%s\n' "$1"; }

have() { command -v "$1" >/dev/null 2>&1; }

# ---------- 基础工具链 ----------
head_ "[1/6] 基础工具链"

if have cmake; then
  v=$(cmake --version | head -1 | awk '{print $3}')
  ok "cmake $v"
else
  bad "缺少 cmake（需要 >= 3.20）"
fi

if have ninja; then
  ok "ninja $(ninja --version)"
else
  warn "缺少 ninja，构建会退回 make（更慢）"
fi

if have make; then
  ok "make $(make --version | head -1 | awk '{print $3}')"
else
  bad "缺少 make"
fi

if have python3; then
  ok "python3 $(python3 --version 2>&1 | awk '{print $2}')"
else
  bad "缺少 python3（DuckDB 扩展构建脚本需要）"
fi

if have git; then
  ok "git $(git --version | awk '{print $3}')"
else
  bad "缺少 git"
fi

# ---------- C++ 编译器 ----------
head_ "[2/6] C++ 编译器（Extension）"

CXX_BIN="${CXX:-c++}"
if have "$CXX_BIN"; then
  if "$CXX_BIN" --version 2>&1 | grep -qi "clang\|gcc\|g++"; then
    ok "$("$CXX_BIN" --version 2>&1 | head -1)"
  else
    warn "$CXX_BIN 存在但无法识别版本"
  fi
else
  bad "缺少 C++ 编译器（安装 Xcode Command Line Tools）"
fi

# ---------- Java / Maven ----------
head_ "[3/6] Java 与 Maven（Gateway）"

if have java; then
  java_major=$(java -version 2>&1 | head -1 | sed -E 's/.*"([0-9]+).*/\1/')
  case "$java_major" in
    11) ok "JDK $(java -version 2>&1 | head -1 | sed -E 's/"//g')" ;;
    8)  warn "JDK 8 可用，但本仓库基线为 JDK 11" ;;
    17|21) warn "JDK $java_major 可用；HBase 2.6 官方支持 8/11，建议 HBase 侧用 11" ;;
    *)  warn "JDK 版本 $java_major 未经验证（建议 11）" ;;
  esac
else
  bad "缺少 JDK（建议 Temurin 11）"
fi

if have mvn; then
  mvn_v=$(mvn -version 2>/dev/null | head -1 | awk '{print $3}')
  ok "maven $mvn_v"
else
  bad "缺少 maven（需要 >= 3.8）"
fi

if [ -n "${JAVA_HOME:-}" ]; then
  ok "JAVA_HOME=$JAVA_HOME"
else
  warn "未设置 JAVA_HOME（HBase 启动脚本依赖它）"
fi

# ---------- DuckDB ----------
head_ "[4/6] DuckDB CLI（运行 Extension）"

if have duckdb; then
  dv=$(duckdb --version 2>/dev/null | awk '{print $1}')
  case "$dv" in
    v2.0.*|2.0.*) ok "duckdb $dv（与构建基线一致）" ;;
    *) warn "duckdb $dv；扩展构建基线为 v2.0.0，版本不一致会导致 LOAD 失败" ;;
  esac
else
  bad "缺少 duckdb CLI（需要 v2.0.0）"
fi

# ---------- HBase ----------
head_ "[5/6] HBase 测试环境"

if have hbase; then
  ok "hbase 命令可用：$(command -v hbase)"
  if timeout 20 hbase version >/dev/null 2>&1; then
    ok "hbase version 可执行"
  else
    warn "hbase version 执行失败（检查 JAVA_HOME 与 HBASE_HOME）"
  fi
  if timeout 30 hbase shell -n <<< "status 'summary'" >/dev/null 2>&1; then
    ok "HBase 集群可连接（hbase shell status 成功）"
  else
    warn "HBase 集群未启动或不可连接：先执行 start-hbase.sh"
  fi
else
  bad "缺少 hbase 命令（安装 HBase 2.6.6 并加入 PATH）"
fi

for p in 2181 16000 16010 16020; do
  if lsof -nP -iTCP:"$p" -sTCP:LISTEN >/dev/null 2>&1; then
    ok "端口 $p 已监听"
  else
    warn "端口 $p 未监听（HBase 未启动时正常）"
  fi
done

# ---------- 资源 ----------
head_ "[6/6] 磁盘与内存"

avail_gb=$(df -g / | tail -1 | awk '{print $4}')
if [ "${avail_gb:-0}" -ge 20 ]; then
  ok "可用磁盘 ${avail_gb}GB（构建 + 依赖建议 >= 20GB）"
else
  warn "可用磁盘 ${avail_gb}GB，可能不足以完成 vcpkg/Java 依赖构建"
fi

mem_gb=$(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1073741824 ))
if [ "$mem_gb" -ge 8 ]; then
  ok "内存 ${mem_gb}GB（建议构建时 -j4/-j6，避免 OOM）"
else
  warn "内存 ${mem_gb}GB，构建大型 C++ 依赖可能 OOM"
fi

# ---------- 汇总 ----------
head_ "汇总"
printf '  PASS=%d  WARN=%d  FAIL=%d\n' "$PASS" "$WARN" "$FAIL"

if [ "$FAIL" -gt 0 ]; then
  printf '\n结论：存在阻塞项，请先修复 FAIL。\n'
  exit 1
fi
if [ "$STRICT" -eq 1 ] && [ "$WARN" -gt 0 ]; then
  printf '\n结论：strict 模式下 WARN 视为失败。\n'
  exit 2
fi
printf '\n结论：环境可用于开发；WARN 项按需补齐（HBase 未启动时出现端口/连接 WARN 属正常）。\n'
