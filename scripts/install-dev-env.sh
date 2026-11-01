#!/usr/bin/env bash
# 安装 DuckDB-HBase 本地开发环境（macOS，用户级安装，无需 sudo）。
#
# 组件：JDK 11 (Temurin 11.0.32.1) / Maven 3.9.16 / HBase 2.6.6 (standalone) / DuckDB CLI 2.0.0
# 文档：docs/environment.md
#
# 用法：
#   bash scripts/install-dev-env.sh                 # 装到 ~/tools，并写入 /usr/local/bin 入口与 ~/.zshrc
#   bash scripts/install-dev-env.sh --no-global     # 只装到安装前缀，不改 /usr/local/bin 与 ~/.zshrc
#   DEV_PREFIX=/opt/devenv bash scripts/install-dev-env.sh
#   bash scripts/install-dev-env.sh -h
#
# 幂等：已完整下载的产物与已解压的组件会跳过；HBase 配置块可重复覆盖，不会重复追加。
#
# 与 scripts/README.md 的 `set -euo pipefail` 约定有意偏差：本脚本不因单步失败中断，
# 而是收集全部失败到末尾统一汇总，避免一次只暴露一个问题（网络/权限/镜像都可能是变量）。
set -uo pipefail

PREFIX="${DEV_PREFIX:-$HOME/tools}"
GLOBAL=1                                      # 1=写 /usr/local/bin 与 ~/.zshrc
JDK_VER="11.0.32.1"
JDK_DIR="jdk11"
MVN_VER="3.9.16"
HBASE_VER="2.6.6"
DUCK_VER="2.0.0"
FAILED=0

usage() {
  sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix)    PREFIX="${2:?--prefix 需要目录}"; shift 2 ;;
    --prefix=*)  PREFIX="${1#*=}"; shift ;;
    --no-global) GLOBAL=0; shift ;;
    -h|--help)   usage; exit 0 ;;
    *)           printf '未知参数：%s（用 -h 查看用法）\n' "$1" >&2; exit 2 ;;
  esac
done

say()  { printf '\n===== %s =====\n' "$*"; }
info() { printf '  %s\n' "$*"; }
fail() { printf '  !! FAIL: %s\n' "$*"; FAILED=$((FAILED + 1)); }

case "$(uname -s)" in
  Darwin) ;;
  *) printf '仅支持 macOS（当前 %s）。Linux 请参考 docs/environment.md 的待定（TBD）小节。\n' "$(uname -s)" >&2; exit 2 ;;
esac
case "$(uname -m)" in
  x86_64) PLAT=x64;    DUCK_ARCH=amd64  ;;
  arm64)  PLAT=aarch64; DUCK_ARCH=arm64 ;;
  *) printf '不支持的 CPU 架构：%s\n' "$(uname -m)" >&2; exit 2 ;;
esac

DL="$PREFIX/downloads"
JDK_TGZ="$DL/OpenJDK11U-jdk_${PLAT}_mac_hotspot_${JDK_VER}_1.tar.gz"
MVN_TGZ="$DL/apache-maven-${MVN_VER}-bin.tar.gz"
HBASE_TGZ="$DL/hbase-${HBASE_VER}-bin.tar.gz"
DUCK_TGZ="$DL/duckdb-cli-osx-${DUCK_ARCH}.tar.gz"

# 完整性判定：能通过 gzip -t 才算完整（下载被中断的包会在这里失败）
intact() { [ -s "$1" ] && gzip -t "$1" 2>/dev/null; }

# fetch <url> <out> <期望字节数下限>
fetch() {
  local url="$1" out="$2" min=${3:-1000000}
  if intact "$out"; then
    info "已存在且校验通过，跳过下载：$(basename "$out")（$(( $(stat -f%z "$out") / 1048576 ))MB）"
    return 0
  fi
  rm -f "$out"
  info "下载 $(basename "$out")"
  info "  <- $url"
  if curl -fL -sS --retry 2 --retry-delay 3 --connect-timeout 20 -o "$out" "$url"; then
    local got; got=$(stat -f%z "$out")
    if [ "$got" -ge "$min" ] && intact "$out"; then
      info "  完成：$((got / 1048576))MB（gzip 校验通过）"
      return 0
    fi
    fail "$(basename "$out") 不完整或损坏：${got}B"
    return 1
  fi
  fail "下载失败：$url"
  return 1
}

# 用 markered 块写入文件：先删除同名块再追加，保证重复执行不产生重复配置
write_block() {                              # write_block <file> <marker> <content>
  local file="$1" marker="$2" content="$3"
  if [ -f "$file" ]; then
    sed -i '' "/^# >>> ${marker} >>>$/,/^# <<< ${marker} <<<$/d" "$file"
  else
    mkdir -p "$(dirname "$file")" && : > "$file"
  fi
  printf '\n# >>> %s >>>\n%s\n# <<< %s <<<\n' "$marker" "$content" "$marker" >> "$file"
}

printf 'DuckDB-HBase 开发环境安装\n  安装前缀：%s\n  平台：macOS %s（%s）\n  全局入口：%s\n' \
  "$PREFIX" "$(uname -m)" "$PLAT" "$([ "$GLOBAL" = 1 ] && echo 写入 || echo 跳过)"

mkdir -p "$PREFIX/bin" "$DL" "$PREFIX/jdks" "$PREFIX/hbase-data"

# ---------------------------------------------------------------- 1. 下载
say "1/8 下载产物"
fetch "https://github.com/adoptium/temurin11-binaries/releases/download/jdk-${JDK_VER}%2B1/OpenJDK11U-jdk_${PLAT}_mac_hotspot_${JDK_VER}_1.tar.gz" "$JDK_TGZ" 150000000
fetch "https://archive.apache.org/dist/maven/maven-3/${MVN_VER}/binaries/apache-maven-${MVN_VER}-bin.tar.gz" "$MVN_TGZ" 9000000
# HBase：官方归档站实测 ~160KB/s，优先华为云镜像，失败再回退官方
if ! fetch "https://mirrors.huaweicloud.com/apache/hbase/${HBASE_VER}/hbase-${HBASE_VER}-bin.tar.gz" "$HBASE_TGZ" 300000000; then
  fetch "https://archive.apache.org/dist/hbase/${HBASE_VER}/hbase-${HBASE_VER}-bin.tar.gz" "$HBASE_TGZ" 300000000
fi
fetch "https://github.com/duckdb/duckdb/releases/download/v${DUCK_VER}/duckdb-cli-osx-${DUCK_ARCH}.tar.gz" "$DUCK_TGZ" 15000000

say "2/8 校验压缩包完整性"
for f in "$JDK_TGZ" "$MVN_TGZ" "$HBASE_TGZ" "$DUCK_TGZ"; do
  if intact "$f"; then info "OK  $(basename "$f")"; else fail "损坏或不完整：$(basename "$f")"; fi
done

# ---------------------------------------------------------------- 3. JDK
say "3/8 安装 JDK 11（Temurin ${JDK_VER}）"
if [ ! -x "$PREFIX/$JDK_DIR/bin/java" ]; then
  tar -xzf "$JDK_TGZ" -C "$PREFIX/jdks" || fail "JDK 解压失败"
  # Temurin 的 macOS 包是 bundle 结构：<bundle>/Contents/Home，JAVA_HOME 必须指向 Contents/Home
  jhome=$(find "$PREFIX/jdks" -maxdepth 4 -type d -name Home -path '*Contents/Home' | head -1)
  if [ -n "${jhome:-}" ]; then ln -sfn "$jhome" "$PREFIX/$JDK_DIR"; else fail "未找到 Contents/Home"; fi
fi
JDK_HOME="$PREFIX/$JDK_DIR"
[ -x "$JDK_HOME/bin/java" ] && info "JAVA_HOME=$JDK_HOME"

# 注册到 macOS 的 JVM 目录：/usr/bin/java 与 /usr/libexec/java_home 因此可发现 JDK 11。
# 注意这里指向 bundle 根（Contents/Home 的上一级），java_home 只认这种结构。
if [ "$GLOBAL" = 1 ]; then
  say "4/8 注册到 ~/Library/Java/JavaVirtualMachines"
  JVM_DIR="$HOME/Library/Java/JavaVirtualMachines"
  BUNDLE_ROOT=$(cd "$JDK_HOME/../.." 2>/dev/null && pwd -P)
  mkdir -p "$JVM_DIR" && ln -sfn "$BUNDLE_ROOT" "$JVM_DIR/temurin-11.jdk"
  /usr/libexec/java_home -v 11 2>/dev/null | sed 's/^/  java_home -v 11 -> /' || fail "java_home 未识别 JDK 11"
else
  say "4/8 跳过 JVM 注册（--no-global）"
fi

# ---------------------------------------------------------------- 4. Maven
say "5/8 安装 Maven ${MVN_VER}"
[ -x "$PREFIX/apache-maven-${MVN_VER}/bin/mvn" ] || tar -xzf "$MVN_TGZ" -C "$PREFIX" || fail "Maven 解压失败"
MAVEN_HOME="$PREFIX/apache-maven-${MVN_VER}"

# ---------------------------------------------------------------- 5. DuckDB
say "6/8 安装 DuckDB CLI ${DUCK_VER}"
if [ ! -x "$PREFIX/bin/duckdb" ]; then
  tar -xzf "$DUCK_TGZ" -C "$PREFIX/bin" || fail "DuckDB 解压失败"
  if [ ! -f "$PREFIX/bin/duckdb" ]; then
    found=$(find "$PREFIX/bin" -maxdepth 3 -type f -name duckdb | head -1)
    [ -n "${found:-}" ] && mv -f "$found" "$PREFIX/bin/duckdb"
  fi
  xattr -dr com.apple.quarantine "$PREFIX/bin/duckdb" 2>/dev/null
  chmod +x "$PREFIX/bin/duckdb"
fi

# ---------------------------------------------------------------- 6. HBase
say "7/8 安装并配置 HBase ${HBASE_VER}（standalone）"
HB="$PREFIX/hbase-${HBASE_VER}"
[ -x "$HB/bin/hbase" ] || tar -xzf "$HBASE_TGZ" -C "$PREFIX" || fail "HBase 解压失败"

# 6a. 数据目录固定到安装前缀下，避免落在 /tmp 被系统清理
cat > "$HB/conf/hbase-site.xml" <<XML
<?xml version="1.0"?>
<?xml-stylesheet type="text/xsl" href="configuration.xsl"?>
<!-- 由 scripts/install-dev-env.sh 生成：本地 standalone 测试配置 -->
<configuration>
  <property><name>hbase.rootdir</name><value>file://${PREFIX}/hbase-data</value></property>
  <property><name>hbase.tmp.dir</name><value>${PREFIX}/hbase-data/tmp</value></property>
  <property><name>hbase.zookeeper.property.dataDir</name><value>${PREFIX}/hbase-data/zookeeper</value></property>
  <property><name>hbase.cluster.distributed</name><value>false</value></property>
  <property><name>hbase.wal.provider</name><value>filesystem</value></property>
  <property><name>hbase.unsafe.stream.capability.enforce</name><value>false</value></property>
</configuration>
XML

# 6b. JDK 11 的 --add-opens：只保留在 JDK 11 上真实存在的包，
#     照抄网上的完整清单会给出一堆 "package ... not in java.base" 噪音警告。
JDK11_OPENS="--add-opens=java.base/java.lang=ALL-UNNAMED"
JDK11_OPENS="$JDK11_OPENS --add-opens=java.base/java.util=ALL-UNNAMED"
JDK11_OPENS="$JDK11_OPENS --add-opens=java.base/java.util.concurrent=ALL-UNNAMED"
JDK11_OPENS="$JDK11_OPENS --add-opens=java.base/java.util.concurrent.atomic=ALL-UNNAMED"
JDK11_OPENS="$JDK11_OPENS --add-opens=java.base/java.io=ALL-UNNAMED"
JDK11_OPENS="$JDK11_OPENS --add-opens=java.base/java.net=ALL-UNNAMED"
JDK11_OPENS="$JDK11_OPENS --add-opens=java.base/java.nio=ALL-UNNAMED"
JDK11_OPENS="$JDK11_OPENS --add-opens=java.base/java.security=ALL-UNNAMED"
JDK11_OPENS="$JDK11_OPENS --add-opens=java.base/javax.security.auth.callback=ALL-UNNAMED"
JDK11_OPENS="$JDK11_OPENS --add-opens=java.base/javax.security.auth.login=ALL-UNNAMED"
JDK11_OPENS="$JDK11_OPENS --add-opens=java.base/javax.net.ssl=ALL-UNNAMED"
JDK11_OPENS="$JDK11_OPENS --add-opens=java.base/sun.nio.ch=ALL-UNNAMED"
JDK11_OPENS="$JDK11_OPENS --add-opens=java.security.sasl/javax.security.sasl=ALL-UNNAMED"
JDK11_OPENS="$JDK11_OPENS --add-opens=java.security.jgss/org.ietf.jgss=ALL-UNNAMED"

write_block "$HB/conf/hbase-env.sh" "DuckDB-HBase local dev" "# 由 scripts/install-dev-env.sh 生成
export JAVA_HOME=\"$JDK_HOME\"
export HBASE_MANAGES_ZK=true
export HBASE_LOG_DIR=\"$PREFIX/hbase-data/logs\"
HBASE_JDK11_OPENS=\"$JDK11_OPENS\"
export HBASE_OPTS=\"\$HBASE_OPTS \$HBASE_JDK11_OPENS\"
export HBASE_MASTER_OPTS=\"\$HBASE_MASTER_OPTS \$HBASE_JDK11_OPENS\"
export HBASE_REGIONSERVER_OPTS=\"\$HBASE_REGIONSERVER_OPTS \$HBASE_JDK11_OPENS\""
info "已写入 hbase-env.sh（JAVA_HOME + JDK11 add-opens）与 hbase-site.xml"

# ---------------------------------------------------------------- 7. 全局入口
say "8/8 配置入口"
if [ "$GLOBAL" = 1 ]; then
  # mvn / hbase 用包装脚本而非软链：它们依赖 JAVA_HOME / HBASE_HOME，
  # 且 start-hbase.sh 之类脚本用 dirname $0 定位 HBASE_HOME，软链会指错目录。
  if [ -w /usr/local/bin ]; then
    cat > /usr/local/bin/mvn <<SH
#!/bin/bash
# DuckDB-HBase dev env: Maven ${MVN_VER} on JDK 11
export JAVA_HOME="$JDK_HOME"
exec "$MAVEN_HOME/bin/mvn" "\$@"
SH
    cat > /usr/local/bin/hbase <<SH
#!/bin/bash
# DuckDB-HBase dev env: HBase ${HBASE_VER} (standalone, JDK 11)
export JAVA_HOME="$JDK_HOME"
export HBASE_HOME="$HB"
exec "$HB/bin/hbase" "\$@"
SH
    for c in start-hbase.sh stop-hbase.sh; do
      cat > "/usr/local/bin/$c" <<SH
#!/bin/bash
# DuckDB-HBase dev env: $c (HBase ${HBASE_VER} standalone, JDK 11)
export JAVA_HOME="$JDK_HOME"
export HBASE_HOME="$HB"
exec "$HB/bin/$c" "\$@"
SH
    done
    chmod +x /usr/local/bin/mvn /usr/local/bin/hbase /usr/local/bin/start-hbase.sh /usr/local/bin/stop-hbase.sh
    ln -sfn "$PREFIX/bin/duckdb" /usr/local/bin/duckdb
    info "已写入 /usr/local/bin/{mvn,hbase,start-hbase.sh,stop-hbase.sh,duckdb}"
  else
    fail "/usr/local/bin 不可写：跳过全局入口（依赖 ~/.zshrc 的 PATH）"
  fi

  ZSHRC="$HOME/.zshrc"
  write_block "$ZSHRC" "duckdb-hbase toolchain" "# JDK 11 + Maven ${MVN_VER} + HBase ${HBASE_VER} + DuckDB ${DUCK_VER}
export JAVA_HOME=\"$JDK_HOME\"
export MAVEN_HOME=\"$MAVEN_HOME\"
export HBASE_HOME=\"$HB\"
export PATH=\"\$JAVA_HOME/bin:\$MAVEN_HOME/bin:\$HBASE_HOME/bin:$PREFIX/bin:\$PATH\""
  info "已写入 $ZSHRC（交互式 zsh 生效：新开终端或 source ~/.zshrc）"
else
  info "--no-global：跳过 /usr/local/bin 入口与 ~/.zshrc"
fi

# ---------------------------------------------------------------- 8. 验证
say "验证：版本输出"
export JAVA_HOME="$JDK_HOME" MAVEN_HOME="$MAVEN_HOME" HBASE_HOME="$HB"
export PATH="$JDK_HOME/bin:$MAVEN_HOME/bin:$HB/bin:$PREFIX/bin:$PATH"
printf '  %-8s %s\n' java   "$(java -version 2>&1 | head -1)"
printf '  %-8s %s\n' javac  "$(javac -version 2>&1)"
printf '  %-8s %s\n' maven  "$(mvn -v 2>/dev/null | head -1)"
printf '  %-8s %s\n' hbase  "$(hbase version 2>/dev/null | head -1)"
printf '  %-8s %s\n' duckdb "$(duckdb --version 2>/dev/null)"

say "清理下载缓存"
rm -rf "$DL"

printf '\n===== 汇总：FAILED=%s =====\n' "$FAILED"
if [ "$FAILED" -eq 0 ]; then
  cat <<TIP
安装完成。下一步：
  start-hbase.sh                      # 启动 standalone HBase
  bash scripts/check-env.sh           # 环境自检
  bash scripts/hbase-fixture.sh       # 建测试夹具表
详见 docs/environment.md。
TIP
fi
[ "$FAILED" -gt 0 ] && exit 1
exit 0
