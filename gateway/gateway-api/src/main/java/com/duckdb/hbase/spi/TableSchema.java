package com.duckdb.hbase.spi;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Objects;

/**
 * 表结构描述。HBase 无固定 schema，qualifier 为空表示该表存在但具体列未知（整列族）。
 *
 * <p>详见 docs/protocol.md 的列映射说明；具体类型映射规则仍为待定（TBD）。
 */
public final class TableSchema {

  private final String namespace;
  private final String table;
  private final List<Column> columns;

  public TableSchema(String namespace, String table, List<Column> columns) {
    this.namespace = namespace;
    this.table = table;
    this.columns = Collections.unmodifiableList(new ArrayList<>(columns));
  }

  public String namespace() {
    return namespace;
  }

  public String table() {
    return table;
  }

  public List<Column> columns() {
    return columns;
  }

  /** 单列定义。 */
  public static final class Column {
    private final String family;
    private final String qualifier;
    private final String sqlType;

    public Column(String family, String qualifier, String sqlType) {
      this.family = Objects.requireNonNull(family, "family");
      this.qualifier = qualifier == null ? "" : qualifier;
      this.sqlType = sqlType == null ? "VARCHAR" : sqlType;
    }

    public String family() {
      return family;
    }

    public String qualifier() {
      return qualifier;
    }

    public String sqlType() {
      return sqlType;
    }

    @Override
    public String toString() {
      return family + (qualifier.isEmpty() ? "" : ":" + qualifier) + " " + sqlType;
    }
  }
}
