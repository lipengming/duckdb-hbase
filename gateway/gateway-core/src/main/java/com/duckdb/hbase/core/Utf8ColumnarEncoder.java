package com.duckdb.hbase.core;

import com.duckdb.hbase.protocol.v1.BatchEncoding;
import com.duckdb.hbase.protocol.v1.ColumnRef;
import com.duckdb.hbase.spi.Row;
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * M1 数据面编码：UTF8 列 + 长度表（大端），无第三方依赖。
 *
 * <p>格式（见 docs/protocol.md §3.4）：
 *
 * <pre>
 * magic      : int32  0x44484231 ('DHB1')
 * columns    : int32
 * rows       : int32
 * 每列元数据 : int32 nameLen + name(UTF8)
 * 每列数据   : int32 length[rows]（-1 表示 NULL）+ 拼接后的 UTF8 字节
 * </pre>
 *
 * <p>目标格式是 Arrow IPC（{@code BATCH_ENCODING_ARROW_IPC}），在 M2 切换；
 * 当前实现保证零依赖、可逐批解码、内存有界。
 */
public final class Utf8ColumnarEncoder {

  private static final int MAGIC = 0x44484231; // "DHB1"

  /** 编码结果。 */
  public static final class EncodedBatch {
    private final byte[] payload;
    private final int rowCount;

    EncodedBatch(byte[] payload, int rowCount) {
      this.payload = payload;
      this.rowCount = rowCount;
    }

    public byte[] payload() {
      return payload;
    }

    public int rowCount() {
      return rowCount;
    }
  }

  public static BatchEncoding encoding() {
    return BatchEncoding.BATCH_ENCODING_UTF8_COLUMNS;
  }

  /** 列名规范：{@code family:qualifier}；qualifier 为空时只用 family。 */
  public static String columnName(ColumnRef column) {
    return column.getQualifier().isEmpty()
        ? column.getFamily()
        : column.getFamily() + ":" + column.getQualifier();
  }

  public EncodedBatch encode(List<ColumnRef> columns, List<Row> rows) throws IOException {
    ByteArrayOutputStream buffer = new ByteArrayOutputStream(64 * 1024);
    DataOutputStream out = new DataOutputStream(buffer);

    out.writeInt(MAGIC);
    out.writeInt(columns.size());
    out.writeInt(rows.size());

    for (ColumnRef column : columns) {
      byte[] name = columnName(column).getBytes(StandardCharsets.UTF_8);
      out.writeInt(name.length);
      out.write(name);
    }

    // 每行只建一次索引，避免 O(列数 x 列数) 的重复扫描
    java.util.List<Map<String, Row.Cell>> rowCells = new java.util.ArrayList<>(rows.size());
    for (Row row : rows) {
      rowCells.add(indexCells(row));
    }

    for (ColumnRef column : columns) {
      String name = columnName(column);
      byte[][] values = new byte[rows.size()][];
      for (int i = 0; i < rows.size(); i++) {
        Row.Cell cell = rowCells.get(i).get(name);
        // Cell 不存在 → NULL；Cell 存在但值为空 → 空字符串
        values[i] = (cell == null || cell.value() == null) ? null : cell.value();
      }
      for (byte[] value : values) {
        out.writeInt(value == null ? -1 : value.length);
      }
      for (byte[] value : values) {
        if (value != null) {
          out.write(value);
        }
      }
    }

    out.flush();
    int nulls = 0;
    for (int i = 0; i < rows.size(); i++) {
      // 诊断：统计编码进 payload 的 NULL 数
    }
    if (System.getenv("DH_ENCODER_DEBUG") != null) {
      System.err.println("[encoder] cols=" + columns.size() + " rows=" + rows.size());
    }
    return new EncodedBatch(buffer.toByteArray(), rows.size());
  }

  private static Map<String, Row.Cell> indexCells(Row row) {
    Map<String, Row.Cell> cells = new HashMap<>();
    for (Row.Cell cell : row.cells()) {
      String family = new String(cell.family(), StandardCharsets.UTF_8);
      String qualifier = new String(cell.qualifier(), StandardCharsets.UTF_8);
      String key = qualifier.isEmpty() ? family : family + ":" + qualifier;
      // HBase 按时间戳倒序返回，保留第一个（最新版本）
      cells.putIfAbsent(key, cell);
    }
    return cells;
  }
}
