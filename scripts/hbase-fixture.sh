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

MAIN="duckdb_test:customer_profile"
EDGE="duckdb_test:edge_cases"

echo "==> 准备 HBase 测试夹具：${MAIN} / ${EDGE}"

hbase shell -n >/tmp/hbase-fixture.log 2>&1 <<RUBY
namespace = 'duckdb_test'
main = '${MAIN}'
edge = '${EDGE}'
keep = ${KEEP}

begin
  create_namespace namespace
  puts "namespace #{namespace} created"
rescue => e
  puts "namespace #{namespace} exists"
end

[main, edge].each do |t|
  next unless exists(t)
  if keep == 1
    puts "keep existing table #{t}"
  else
    disable t
    drop t
    puts "dropped #{t}"
  end
end

create main, {NAME => 'cf1', VERSIONS => 3}, {NAME => 'cf2', VERSIONS => 3} unless exists(main)
create edge, {NAME => 'cf1', VERSIONS => 1}, {NAME => 'cf2', VERSIONS => 1} unless exists(edge)

# 主表：100 行；r007 缺 phone；r011 的 cf2:score 有两个版本
(1..100).each do |i|
  rk = format('r%03d', i)
  put main, rk, 'cf1:name', "user-#{format('%03d', i)}"
  put main, rk, 'cf1:city', "city-#{i % 10}"
  put main, rk, 'cf1:phone', "138#{format('%08d', i)}" unless i == 7
  put main, rk, 'cf2:age', (20 + i % 40).to_s
  put main, rk, 'cf2:score', ((i * 3) % 100).to_s
end
put main, 'r011', 'cf2:score', '11', 1000
put main, 'r011', 'cf2:score', '99', 2000

# 边界表：二进制 RowKey / 中文 / 空值 / 超长值 / 宽行
put edge, "\x00\x01\x02".b, 'cf1:bin', 'bin-rowkey'
put edge, 'row|with|pipes', 'cf1:name', 'pipes'
put edge, '行键-中文', 'cf1:name', '中文值'
put edge, 'r-empty', 'cf1:blank', ''
put edge, 'r-long', 'cf1:blob', 'x' * 65536
(1..200).each { |i| put edge, 'r-wide', format('cf1:c%03d', i), "v#{i}" }

puts "rows written"
RUBY

echo "==> 写入完成，校验行数"
hbase shell -n <<RUBY
count '${MAIN}'
count '${EDGE}'
get '${MAIN}', 'r011', {COLUMN => 'cf2:score', VERSIONS => 3}
get '${MAIN}', 'r007'
RUBY

cat <<'TIP'

夹具就绪，可用于：
  - 投影/过滤：cf1:name, cf1:city, cf1:phone, cf2:age, cf2:score
  - 缺失列：r007 无 cf1:phone（NULL 语义）
  - 版本语义：r011 的 cf2:score 有 ts=1000/2000 两版
  - RowKey 边界：duckdb_test:edge_cases 的二进制/中文/超长/宽行

提示：如需重建，直接再跑一次本脚本；加 --keep 可保留已有表。
TIP
