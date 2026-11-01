package com.duckdb.hbase.spi;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Objects;

/** 一行 HBase 数据：RowKey + 若干 Cell。字节数组按 HBase 语义原样保存，不做 UTF-8 假设。 */
public final class Row {

  private final byte[] rowKey;
  private final List<Cell> cells;

  public Row(byte[] rowKey, List<Cell> cells) {
    this.rowKey = Objects.requireNonNull(rowKey, "rowKey").clone();
    this.cells = Collections.unmodifiableList(new ArrayList<>(cells));
  }

  public byte[] rowKey() {
    return rowKey.clone();
  }

  public List<Cell> cells() {
    return cells;
  }

  /** 单个 Cell。value 为 null 表示该列不存在（HBase 语义上的空值）。 */
  public static final class Cell {
    private final byte[] family;
    private final byte[] qualifier;
    private final byte[] value;
    private final long timestamp;

    public Cell(byte[] family, byte[] qualifier, byte[] value, long timestamp) {
      this.family = Objects.requireNonNull(family, "family").clone();
      this.qualifier = Objects.requireNonNull(qualifier, "qualifier").clone();
      this.value = value == null ? null : value.clone();
      this.timestamp = timestamp;
    }

    public byte[] family() {
      return family.clone();
    }

    public byte[] qualifier() {
      return qualifier.clone();
    }

    public byte[] value() {
      return value == null ? null : value.clone();
    }

    public long timestamp() {
      return timestamp;
    }

    @Override
    public String toString() {
      return "Cell{"
          + new String(family)
          + ":"
          + new String(qualifier)
          + ", ts="
          + timestamp
          + ", bytes="
          + (value == null ? 0 : value.length)
          + '}';
    }
  }

  @Override
  public String toString() {
    return "Row{rowKey=" + Arrays.toString(rowKey) + ", cells=" + cells.size() + '}';
  }
}
