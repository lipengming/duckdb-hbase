#!/usr/bin/env bash
# 统一环境启动：拉起 HBase、准备测试夹具、构建并启动 Gateway。幂等，可重复执行。
# 只使用已安装的环境，不执行任何安装动作。
#
# 用法：
#   bash scripts/env-up.sh                 # 默认：HBase + 夹具 + Gateway
#   bash scripts/env-up.sh --no-build      # 不重新构建 Gateway（jar 缺失时仍会构建）
#   bash scripts/env-up.sh --no-gateway    # 只起 HBase 与夹具
#   bash scripts/env-up.sh --no-fixtures   # 不检查/创建夹具表
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD=1
START_GATEWAY=1
FIXTURES=1
for arg in "$@"; do
  case "$arg" in
    --no-build) BUILD=0 ;;
    --no-gateway) START_GATEWAY=0 ;;
    --no-fixtures) FIXTURES=0 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "未知参数: $arg" >&2; exit 2 ;;
  esac
done

RUN_DIR="$ROOT_DIR/run"
GW_PID_FILE="$RUN_DIR/gateway.pid"
GW_LOG="$RUN_DIR/gateway.log"
GW_PORT=18080
GW_CONFIG="$ROOT_DIR/gateway/conf/application.properties"
GW_JAR=$(ls "$ROOT_DIR"/gateway/gateway-server/target/gateway-server-*-SNAPSHOT.jar 2>/dev/null | head -1)

log() { printf '\033[36m==>\033[0m %s\n' "$*"; }
fail() { printf '\033[31mERROR\033[0m %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
port_open() { (exec 3<>"/dev/tcp/127.0.0.1/$1") >/dev/null 2>&1; }
run_timeout() {
  local seconds="$1"; shift
  perl -e 'my $t = shift; $SIG{ALRM} = sub { exit 124 }; alarm $t; system(@ARGV); exit($? >> 8)' "$seconds" "$@"
}

mkdir -p "$RUN_DIR"

# ---------- 0. 环境前置（只检查，不安装） ----------
for cmd in java mvn hbase duckdb; do
  have "$cmd" || fail "缺少 ${cmd}，请先按 docs/environment.md 安装（本脚本不安装环境）"
done

# ---------- 1. HBase ----------
# 先看端口与进程（便宜且不受机器负载影响），必要时再用 hbase shell 兜底
hbase_running() {
  if port_open 2181 && jps 2>/dev/null | grep -q HMaster; then
    return 0
  fi
  run_timeout 90 hbase shell -n <<< "status 'summary'" 2>/dev/null | grep -q "active master"
}

log "检查 HBase ..."
if hbase_running; then
  log "HBase 已在运行"
else
  log "启动 HBase（standalone）..."
  start-hbase.sh >/dev/null 2>&1 || true
  ready=0
  for _ in $(seq 1 45); do
    sleep 2
    if hbase_running; then ready=1; break; fi
  done
  [ "$ready" -eq 1 ] || fail "HBase 90 秒内未就绪，请查看 \$HBASE_HOME/logs"
  log "HBase 已启动"
fi

# ---------- 2. 测试夹具 ----------
if [ "$FIXTURES" -eq 1 ]; then
  log "检查测试夹具 ..."
  fixture_state=$(run_timeout 60 hbase shell -n <<< "exists 'duckdb_test:customer_profile'" 2>/dev/null)
  if printf '%s' "$fixture_state" | grep -q "does exist"; then
    log "夹具表已存在（如需重建：bash scripts/hbase-fixture.sh）"
  elif printf '%s' "$fixture_state" | grep -q "does not exist"; then
    log "创建夹具表与数据 ..."
    bash "$ROOT_DIR/scripts/hbase-fixture.sh" >/dev/null || fail "hbase-fixture.sh 执行失败"
    log "夹具已创建"
  else
    log "无法确认夹具状态（HBase 响应超时），跳过创建；需要时执行 bash scripts/hbase-fixture.sh"
  fi
fi

# ---------- 3. Gateway ----------
if [ "$START_GATEWAY" -eq 1 ]; then
  if port_open "$GW_PORT"; then
    log "Gateway 已在 $GW_PORT 监听，跳过启动"
  else
    if [ -z "$GW_JAR" ] || [ "$BUILD" -eq 1 ]; then
      log "构建 Gateway（mvn -DskipTests package）..."
      mvn -B -q -f gateway/pom.xml -DskipTests package || fail "Gateway 构建失败，请查看上方 Maven 输出"
      GW_JAR=$(ls "$ROOT_DIR"/gateway/gateway-server/target/gateway-server-*-SNAPSHOT.jar 2>/dev/null | head -1)
      [ -n "$GW_JAR" ] || fail "构建完成但未找到 gateway-server jar"
    fi

    log "启动 Gateway：$GW_LOG"
    nohup java -jar "$GW_JAR" "$GW_CONFIG" >"$GW_LOG" 2>&1 &
    echo $! >"$GW_PID_FILE"

    ready=0
    for _ in $(seq 1 30); do
      sleep 1
      if port_open "$GW_PORT"; then ready=1; break; fi
      if ! kill -0 "$(cat "$GW_PID_FILE")" 2>/dev/null; then
        break
      fi
    done
    if [ "$ready" -ne 1 ]; then
      tail -20 "$GW_LOG" >&2
      fail "Gateway 启动失败（日志：${GW_LOG#$ROOT_DIR/}）"
    fi
    log "Gateway 已启动（pid $(cat "$GW_PID_FILE")，端口 ${GW_PORT}）"
  fi
fi

# ---------- 4. 汇总 ----------
printf '\n'
bash "$ROOT_DIR/scripts/env-check.sh" | tail -6
printf '\n下一步：\n'
printf "  DuckDB 验证： ./build/release/duckdb -unsigned -c \"LOAD 'build/release/extension/hbase/hbase.duckdb_extension'; SELECT hbase_version();\"\n"
printf '  停止环境：   bash scripts/env-down.sh\n'
