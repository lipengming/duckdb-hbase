package com.duckdb.hbase.core;

import com.duckdb.hbase.protocol.v1.BatchEncoding;
import com.duckdb.hbase.protocol.v1.ColumnRef;
import com.duckdb.hbase.spi.Row;
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * 数据面编码：UTF8 列 + 长度表（大端），无第三方依赖。
 *
 * <pre>
 * magic      : int32  0x44484231 ('DHB1')
 * columns    : int32
 * rows       : int32
 * 每列元数据 : int32 nameLen + name(UTF8)
 * 每列数据   : int32 length[rows]（-1 表示 NULL）+ 拼接后的 UTF8 字节
 * </pre>
 *
 * <p>max_versions &gt; 1 时按 Cell 时间戳展开，并追加 {@code __timestamp} 列。
 * 目标格式为 Arrow IPC，当前实现为过渡方案，见 docs/roadmap.md 技术债。
 */
public final class Utf8ColumnarEncoder {

  private static final int MAGIC = 0x44484231; // "DHB1"

  /** max_versions > 1 时追加的时间戳列（毫秒字符串，与其它列一样为 VARCHAR）。 */
  public static final String TIMESTAMP_COLUMN = "__timestamp";

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
    return encode(columns, rows, false);
  }

  /**
   * @param expandVersions true 时按 Cell 时间戳展开成多行，并追加 {@link #TIMESTAMP_COLUMN} 列
   */
  public EncodedBatch encode(List<ColumnRef> columns, List<Row> rows, boolean expandVersions)
      throws IOException {
    if (expandVersions) {
      return encodeExpanded(columns, rows);
    }

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
    List<Map<String, Row.Cell>> rowCells = new ArrayList<>(rows.size());
    for (Row row : rows) {
      rowCells.add(indexCells(row));
    }

    for (ColumnRef column : columns) {
      String name = columnName(column);
      List<byte[]> values = new ArrayList<>(rows.size());
      for (Map<String, Row.Cell> cells : rowCells) {
        Row.Cell cell = cells.get(name);
        // Cell 不存在 → NULL；Cell 存在但值为空 → 空字符串
        values.add(cell == null || cell.value() == null ? null : cell.value());
      }
      writeColumn(out, values);
    }

    out.flush();
    return new EncodedBatch(buffer.toByteArray(), rows.size());
  }

  /** 按 (RowKey, Cell 时间戳) 展开：每个时间戳一行，缺失列写 NULL，末尾追加时间戳列。 */
  private EncodedBatch encodeExpanded(List<ColumnRef> columns, List<Row> rows) throws IOException {
    List<String> columnNames = new ArrayList<>(columns.size());
    for (ColumnRef column : columns) {
      columnNames.add(columnName(column));
    }

    List<Map<Long, Map<String, byte[]>>> expanded = new ArrayList<>(rows.size());
    int outputRows = 0;
    for (Row row : rows) {
      Map<Long, Map<String, byte[]>> byVersion = new HashMap<>();
      for (Row.Cell cell : row.cells()) {
        String key = cellName(cell);
        if (!columnNames.contains(key)) {
          continue;
        }
        byVersion
            .computeIfAbsent(cell.timestamp(), ignored -> new HashMap<>())
            .putIfAbsent(key, cell.value());
      }
      if (byVersion.isEmpty()) {
        continue;
      }
      expanded.add(byVersion);
      outputRows += byVersion.size();
    }

    ByteArrayOutputStream buffer = new ByteArrayOutputStream(64 * 1024);
    DataOutputStream out = new DataOutputStream(buffer);
    out.writeInt(MAGIC);
    out.writeInt(columnNames.size() + 1);
    out.writeInt(outputRows);

    for (String name : columnNames) {
      byte[] bytes = name.getBytes(StandardCharsets.UTF_8);
      out.writeInt(bytes.length);
      out.write(bytes);
    }
    byte[] timestampName = TIMESTAMP_COLUMN.getBytes(StandardCharsets.UTF_8);
    out.writeInt(timestampName.length);
    out.write(timestampName);

    for (String name : columnNames) {
      List<byte[]> values = new ArrayList<>(outputRows);
      for (Map<Long, Map<String, byte[]>> byVersion : expanded) {
        for (Long timestamp : sortedTimestamps(byVersion)) {
          values.add(byVersion.get(timestamp).get(name));
        }
      }
      writeColumn(out, values);
    }

    List<byte[]> timestamps = new ArrayList<>(outputRows);
    for (Map<Long, Map<String, byte[]>> byVersion : expanded) {
      for (Long timestamp : sortedTimestamps(byVersion)) {
        timestamps.add(Long.toString(timestamp).getBytes(StandardCharsets.UTF_8));
      }
    }
    writeColumn(out, timestamps);

    out.flush();
    return new EncodedBatch(buffer.toByteArray(), outputRows);
  }

  private static List<Long> sortedTimestamps(Map<Long, Map<String, byte[]>> byVersion) {
    List<Long> timestamps = new ArrayList<>(byVersion.keySet());
    timestamps.sort((a, b) -> Long.compare(b, a)); // 新版本在前
    return timestamps;
  }

  private static void writeColumn(DataOutputStream out, List<byte[]> values) throws IOException {
    for (byte[] value : values) {
      out.writeInt(value == null ? -1 : value.length);
    }
    for (byte[] value : values) {
      if (value != null) {
        out.write(value);
      }
    }
  }

  private static Map<String, Row.Cell> indexCells(Row row) {
    Map<String, Row.Cell> cells = new HashMap<>();
    for (Row.Cell cell : row.cells()) {
      // HBase 按时间戳倒序返回，保留第一个（最新版本）
      cells.putIfAbsent(cellName(cell), cell);
    }
    return cells;
  }

  private static String cellName(Row.Cell cell) {
    String family = new String(cell.family(), StandardCharsets.UTF_8);
    String qualifier = new String(cell.qualifier(), StandardCharsets.UTF_8);
    return qualifier.isEmpty() ? family : family + ":" + qualifier;
  }
}
