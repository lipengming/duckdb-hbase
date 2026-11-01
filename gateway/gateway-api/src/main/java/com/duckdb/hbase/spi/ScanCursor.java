package com.duckdb.hbase.spi;

import java.util.List;

/**
 * Gateway 内部游标，包装 HBase {@code ResultScanner} 之类的底层游标，避免向上层暴露客户端实现。
 *
 * <p>调用方按批拉取：{@link #nextBatch(int)} 返回空列表表示已结束；必须调用 {@link #close()} 释放资源。
 */
public interface ScanCursor extends AutoCloseable {

  /** 对应请求的 request_id，用于取消；未设置时返回空串。 */
  String requestId();

  /**
   * 拉取下一批数据。
   *
   * @param maxRows 本次最多返回的行数，必须为正数
   * @return 行列表；为空表示游标已耗尽
   */
  List<Row> nextBatch(int maxRows) throws HBaseAccessException;

  @Override
  void close() throws HBaseAccessException;
}
