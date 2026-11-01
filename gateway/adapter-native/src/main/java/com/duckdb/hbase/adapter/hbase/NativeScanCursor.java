package com.duckdb.hbase.adapter.hbase;

import com.duckdb.hbase.spi.HBaseAccessException;
import com.duckdb.hbase.spi.Row;
import com.duckdb.hbase.spi.ScanCursor;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import org.apache.hadoop.hbase.Cell;
import org.apache.hadoop.hbase.CellUtil;
import org.apache.hadoop.hbase.client.Result;
import org.apache.hadoop.hbase.client.ResultScanner;
import org.apache.hadoop.hbase.client.Table;

/** 包装 HBase {@code ResultScanner} 的流式游标。 */
final class NativeScanCursor implements ScanCursor {

  private final String requestId;
  private final Table table;
  private final ResultScanner scanner;
  private boolean closed;

  NativeScanCursor(String requestId, Table table, ResultScanner scanner) {
    this.requestId = requestId;
    this.table = table;
    this.scanner = scanner;
  }

  @Override
  public String requestId() {
    return requestId;
  }

  @Override
  public List<Row> nextBatch(int maxRows) throws HBaseAccessException {
    if (maxRows <= 0) {
      throw new IllegalArgumentException("maxRows 必须为正数");
    }
    if (closed) {
      return Collections.emptyList();
    }
    try {
      Result[] results = scanner.next(maxRows);
      if (results == null || results.length == 0) {
        close();
        return Collections.emptyList();
      }
      List<Row> rows = new ArrayList<>(results.length);
      for (Result result : results) {
        rows.add(toRow(result));
      }
      return rows;
    } catch (IOException e) {
      closeQuietly();
      throw new HBaseAccessException("读取 Scan 结果失败", e);
    }
  }

  @Override
  public void close() throws HBaseAccessException {
    if (closed) {
      return;
    }
    closed = true;
    try {
      scanner.close();
    } finally {
      try {
        table.close();
      } catch (IOException e) {
        throw new HBaseAccessException("关闭 Table 失败", e);
      }
    }
  }

  private void closeQuietly() {
    try {
      close();
    } catch (HBaseAccessException ignored) {
      // 关闭失败不覆盖原始错误
    }
  }

  static Row toRow(Result result) {
    Cell[] cells = result.rawCells();
    List<Row.Cell> converted = new ArrayList<>(cells == null ? 0 : cells.length);
    if (cells != null) {
      for (Cell cell : cells) {
        converted.add(
            new Row.Cell(
                CellUtil.cloneFamily(cell),
                CellUtil.cloneQualifier(cell),
                CellUtil.cloneValue(cell),
                cell.getTimestamp()));
      }
    }
    return new Row(result.getRow(), converted);
  }
}
