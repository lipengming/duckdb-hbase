#!/usr/bin/env bash
# 统一环境停止：停止 Gateway（按 run/gateway.pid），可选停止 HBase。
#
# 用法：
#   bash scripts/env-down.sh                # 停 Gateway + HBase
#   bash scripts/env-down.sh --keep-hbase   # 只停 Gateway
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

KEEP_HBASE=0
[ "${1:-}" = "--keep-hbase" ] && KEEP_HBASE=1

RUN_DIR="$ROOT_DIR/run"
GW_PID_FILE="$RUN_DIR/gateway.pid"

log() { printf '\033[36m==>\033[0m %s\n' "$*"; }

# ---------- Gateway ----------
if [ -f "$GW_PID_FILE" ]; then
  pid=$(cat "$GW_PID_FILE")
  if kill -0 "$pid" 2>/dev/null; then
    log "停止 Gateway（pid ${pid}）"
    kill "$pid" 2>/dev/null
    for _ in $(seq 1 10); do
      sleep 1
      kill -0 "$pid" 2>/dev/null || break
    done
    kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
  else
    log "Gateway 进程不存在，清理 pid 文件"
  fi
  rm -f "$GW_PID_FILE"
else
  log "未发现 Gateway pid 文件，跳过"
fi

# ---------- HBase ----------
if [ "$KEEP_HBASE" -eq 1 ]; then
  log "保留 HBase（--keep-hbase）"
elif command -v stop-hbase.sh >/dev/null 2>&1; then
  log "停止 HBase ..."
  stop-hbase.sh >/dev/null 2>&1 || true
  log "HBase 已停止"
else
  log "未找到 stop-hbase.sh，跳过 HBase"
fi

printf '\n完成。查看状态：bash scripts/env-check.sh\n'
