package com.duckdb.hbase.adapter.hbase;

import com.duckdb.hbase.spi.HBaseAccessException;
import com.duckdb.hbase.spi.Row;
import com.duckdb.hbase.spi.ScanCursor;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;
import org.apache.hadoop.hbase.client.Result;
import org.apache.hadoop.hbase.client.Table;

/**
 * BatchGet 结果游标。
 *
 * <p>HBase {@code Table.get(List<Get>)} 一次性返回全部结果， 因此这里按批切分后即关闭 Table，避免长期占用。
 */
final class NativeGetCursor implements ScanCursor {

  private final String requestId;
  private final Table table;
  private final List<Result> results;
  private final Consumer<ScanCursor> onClose;
  private final AtomicBoolean closed = new AtomicBoolean();
  private int position;

  NativeGetCursor(String requestId, Table table, Result[] results) {
    this(requestId, table, results, null);
  }

  NativeGetCursor(
      String requestId, Table table, Result[] results, Consumer<ScanCursor> onClose) {
    this.requestId = requestId;
    this.table = table;
    this.onClose = onClose;
    this.results = new ArrayList<>(results == null ? 0 : results.length);
    if (results != null) {
      Collections.addAll(this.results, results);
    }
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
    if (closed.get()) {
      return Collections.emptyList();
    }
    int end = Math.min(position + maxRows, results.size());
    List<Row> rows = new ArrayList<>(Math.max(0, end - position));
    for (int i = position; i < end; i++) {
      rows.add(NativeScanCursor.toRow(results.get(i)));
    }
    position = end;
    if (position >= results.size()) {
      close();
    }
    return rows;
  }

  @Override
  public void close() throws HBaseAccessException {
    if (!closed.compareAndSet(false, true)) {
      return;
    }
    try {
      table.close();
    } catch (IOException e) {
      throw new HBaseAccessException("关闭 Table 失败", e);
    } finally {
      if (onClose != null) {
        onClose.accept(this);
      }
    }
  }
}
