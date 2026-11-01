#!/usr/bin/env bash
# 在本地 HBase 上创建 DuckDB-HBase 集成测试所需的夹具表与数据。
# 用法：bash scripts/hbase-fixture.sh [--keep]
#   --keep   已存在的表不重建（默认先 drop 再 create，保证可重复执行）
# 前置：HBase 2.6.x 已启动（standalone 即可），`hbase` 在 PATH 中。
set -euo pipefail

KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

if ! command -v hbase >/dev/null 2>&1; then
  echo "ERROR: 未找到 hbase 命令，请先安装 HBase 2.6.x 并加入 PATH" >&2
  exit 1
fi

NAMESPACE="duckdb_test"
TABLE_MAIN="${NAMESPACE}:customer_profile"
TABLE_EDGE="${NAMESPACE}:edge_cases"

echo "==> 准备 HBase 测试夹具：${TABLE_MAIN} / ${TABLE_EDGE}"

if [ "$KEEP" -eq 0 ]; then
  hbase shell -n >/dev/null 2>&1 <<EOF || true
disable '${TABLE_MAIN}' rescue => nil
drop '${TABLE_MAIN}' rescue => nil
disable '${TABLE_EDGE}' rescue => nil
drop '${TABLE_EDGE}' rescue => nil
EOF
fi

echo "==> 创建 namespace 与表（3 个版本，便于验证版本语义）"
hbase shell -n <<EOF
create_namespace '${NAMESPACE}' rescue => nil
create '${TABLE_MAIN}', {NAME => 'cf1', VERSIONS => 3}, {NAME => 'cf2', VERSIONS => 3} rescue => nil
create '${TABLE_EDGE}', {NAME => 'cf1', VERSIONS => 1}, {NAME => 'cf2', VERSIONS => 1} rescue => nil
EOF

echo "==> 写入主表数据（100 行；r0007 缺 phone，r0011 有两版 score）"
{
  for i in $(seq -w 1 100); do
    rk="r0${i}"
    printf "put '%s', '%s', 'cf1:name', 'user-%s'\n" "$TABLE_MAIN" "$rk" "$i"
    printf "put '%s', '%s', 'cf1:city', 'city-%s'\n" "$TABLE_MAIN" "$rk" "$((i % 10))"
    if [ "$i" != "07" ]; then
      printf "put '%s', '%s', 'cf1:phone', '138%08d'\n" "$TABLE_MAIN" "$rk" "$i"
    fi
    printf "put '%s', '%s', 'cf2:age', '%s'\n" "$TABLE_MAIN" "$rk" "$((20 + i % 40))"
    printf "put '%s', '%s', 'cf2:score', '%s'\n" "$TABLE_MAIN" "$rk" "$((i * 3 % 100))"
  done
  # 同一 cell 的两个版本：timestamp 显式指定，验证 max_versions / timestamp 过滤
  printf "put '%s', 'r0011', 'cf2:score', '11', 1000\n" "$TABLE_MAIN"
  printf "put '%s', 'r0011', 'cf2:score', '99', 2000\n" "$TABLE_MAIN"
} > /tmp/hbase-fixture-main.rb

echo "==> 写入边界表数据（二进制 RowKey / 空值 / 超长值 / 宽行）"
cat > /tmp/hbase-fixture-edge.rb <<'RUBY'
table = 'duckdb_test:edge_cases'
# 二进制 RowKey（不可按 UTF-8 解释）
put table, "\x00\x01\x02", 'cf1:bin', 'bin-rowkey'
# 带分隔符与中文的 RowKey
put table, "row|with|pipes", 'cf1:name', 'pipes'
put table, "行键-中文", 'cf1:name', '中文值'
# 空字符串值 与 只有 RowKey 的行
put table, 'r-empty', 'cf1:blank', ''
put table, 'r-rowkey-only', 'cf1:never', 'not-used'
# 超长值（约 64KB）
put table, 'r-long', 'cf1:blob', 'x' * 65536
# 宽行：200 个 qualifier
(1..200).each { |i| put table, 'r-wide', "cf1:c#{format('%03d', i)}", "v#{i}" }
RUBY

cat /tmp/hbase-fixture-main.rb /tmp/hbase-fixture-edge.rb | hbase shell -n >/tmp/hbase-fixture.log 2>&1

echo "==> 数据写入完成，开始校验"
hbase shell -n <<EOF
count '${TABLE_MAIN}'
count '${TABLE_EDGE}'
get '${TABLE_MAIN}', 'r0011', {COLUMN => 'cf2:score', VERSIONS => 3}
EOF

rm -f /tmp/hbase-fixture-main.rb /tmp/hbase-fixture-edge.rb

cat <<'TIP'

夹具就绪，可用于：
  - 投影/过滤：cf1:name, cf1:city, cf1:phone, cf2:age, cf2:score
  - 缺失列：r0007 无 cf1:phone（NULL 语义）
  - 版本语义：r0011 的 cf2:score 有 ts=1000/2000 两版
  - RowKey 边界：duckdb_test:edge_cases 的二进制/中文/超长/宽行

提示：如需重建，直接再跑一次本脚本；加 --keep 可保留已有表。
TIP
