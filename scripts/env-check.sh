#!/usr/bin/env bash
# 统一环境检查：只检查、不安装、不修改。退出码 0=可用，1=存在阻塞项。
#
# 用法：
#   bash scripts/env-check.sh           # 人读输出
#   bash scripts/env-check.sh --quiet   # 只输出汇总与失败项（CI/脚本用）
set -uo pipefail

QUIET=0
[ "${1:-}" = "--quiet" ] && QUIET=1

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PASS=0
WARN=0
FAIL=0

ok()   { PASS=$((PASS + 1)); [ "$QUIET" -eq 0 ] && printf '  \033[32mPASS\033[0m  %s\n' "$1"; return 0; }
warn() { WARN=$((WARN + 1)); printf '  \033[33mWARN\033[0m  %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m  %s\n' "$1"; }
section() { [ "$QUIET" -eq 0 ] && printf '\n%s\n' "$1"; return 0; }

have() { command -v "$1" >/dev/null 2>&1; }

# macOS 没有 timeout，用 perl 兜底（超时返回 124，不打印信号噪音）
run_timeout() {
  local seconds="$1"; shift
  perl -e 'my $t = shift; $SIG{ALRM} = sub { exit 124 }; alarm $t; system(@ARGV); exit($? >> 8)' \
    "$seconds" "$@"
}

port_open() { (exec 3<>"/dev/tcp/127.0.0.1/$1") >/dev/null 2>&1; }

# ---------- 1. 工具链（只检查，不安装） ----------
section "[1/5] 工具链"

if have java; then
  major=$(java -version 2>&1 | head -1 | sed -E 's/.*"([0-9]+).*/\1/')
  [ "$major" = "11" ] && ok "JDK 11" || warn "JDK ${major}（基线 11，可能仍可编译）"
else
  bad "缺少 java：参见 docs/environment.md（本脚本不安装）"
fi

if have mvn; then ok "maven $(mvn -version 2>/dev/null | head -1 | awk '{print $3}')"; else bad "缺少 maven：参见 docs/environment.md"; fi
if have hbase; then ok "hbase $(hbase version 2>/dev/null | head -1 | awk '{print $2}')"; else bad "缺少 hbase：参见 docs/environment.md"; fi
if have duckdb; then
  dv=$(duckdb --version 2>/dev/null | awk '{print $1}')
  case "$dv" in v2.0.*|2.0.*) ok "duckdb ${dv}（与扩展基线一致）" ;; *) warn "duckdb ${dv}，扩展基线为 v2.0.0" ;; esac
else
  bad "缺少 duckdb CLI：参见 docs/environment.md"
fi

have cmake && ok "cmake $(cmake --version | head -1 | awk '{print $3}')" || bad "缺少 cmake"
have ninja && ok "ninja $(ninja --version)" || warn "缺少 ninja（构建会退回 make，较慢）"
have python3 && ok "python3 $(python3 --version 2>&1 | awk '{print $2}')" || bad "缺少 python3"
have perl && ok "perl（脚本超时控制用）" || warn "缺少 perl，HBase 探测可能阻塞"

# ---------- 2. HBase 集群 ----------
section "[2/5] HBase"

if port_open 2181; then
  ok "ZooKeeper 端口 2181 已监听"
else
  warn "ZooKeeper 未启动：执行 bash scripts/env-up.sh 拉起 HBase"
fi

if have hbase; then
  hbase_status=$(run_timeout 60 hbase shell -n <<< "status 'summary'" 2>/dev/null)
  if printf '%s' "$hbase_status" | grep -q "active master"; then
    ok "HBase 集群可用（status summary 成功）"
  else
    warn "HBase 未启动或不可连接（bash scripts/env-up.sh）"
  fi
fi

# ---------- 3. 测试夹具 ----------
section "[3/5] 测试夹具"

if have hbase; then
  main_exists=$(run_timeout 60 hbase shell -n <<< "exists 'duckdb_test:customer_profile'" 2>/dev/null)
  edge_exists=$(run_timeout 60 hbase shell -n <<< "exists 'duckdb_test:edge_cases'" 2>/dev/null)
  if printf '%s' "$main_exists" | grep -q "does exist"; then
    ok "duckdb_test:customer_profile 存在"
    if printf '%s' "$edge_exists" | grep -q "does exist"; then
      ok "duckdb_test:edge_cases 存在"
    else
      warn "edge_cases 不存在（bash scripts/hbase-fixture.sh）"
    fi
  else
    warn "夹具表不存在：bash scripts/hbase-fixture.sh"
  fi
fi

# ---------- 4. Gateway ----------
section "[4/5] Gateway"

GW_PID_FILE="$ROOT_DIR/run/gateway.pid"
if port_open 18080; then
  ok "Gateway 端口 18080 可连接"
  if [ -f "$GW_PID_FILE" ] && kill -0 "$(cat "$GW_PID_FILE")" 2>/dev/null; then
    ok "Gateway 进程存活（pid $(cat "$GW_PID_FILE")）"
  else
    warn "18080 有监听但 pid 文件缺失，可能由其他进程占用"
  fi
else
  warn "Gateway 未启动（bash scripts/env-up.sh 会构建并启动）"
fi

GW_JAR=$(ls "$ROOT_DIR"/gateway/gateway-server/target/gateway-server-*-SNAPSHOT.jar 2>/dev/null | head -1)
if [ -n "$GW_JAR" ]; then ok "Gateway 构建产物存在: ${GW_JAR#$ROOT_DIR/}"; else warn "Gateway 尚未构建（mvn -f gateway/pom.xml -DskipTests package）"; fi

# ---------- 5. Extension ----------
section "[5/5] Extension"

EXT_BIN="$ROOT_DIR/build/release/extension/hbase/hbase.duckdb_extension"
if [ -f "$EXT_BIN" ]; then ok "扩展构建产物存在: ${EXT_BIN#$ROOT_DIR/}"; else warn "扩展尚未构建（make release）"; fi

if [ -f "$ROOT_DIR/duckdb/CMakeLists.txt" ]; then ok "duckdb submodule 已就绪"; else bad "duckdb submodule 缺失（git submodule update --init --recursive）"; fi

# ---------- 汇总 ----------
printf '\n汇总: PASS=%d WARN=%d FAIL=%d\n' "$PASS" "$WARN" "$FAIL"
if [ "$FAIL" -gt 0 ]; then
  printf '结论: 存在阻塞项，请先修复 FAIL（安装说明见 docs/environment.md）\n'
  exit 1
fi
printf '结论: 环境可用；WARN 项按需补齐（HBase/Gateway 未启动属正常，用 scripts/env-up.sh 拉起）\n'
