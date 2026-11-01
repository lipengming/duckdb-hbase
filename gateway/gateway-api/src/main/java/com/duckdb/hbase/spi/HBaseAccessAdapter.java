package com.duckdb.hbase.spi;

import com.duckdb.hbase.protocol.v1.BatchGetRequest;
import com.duckdb.hbase.protocol.v1.ScanRequest;

/**
 * Gateway 侧统一访问接口（技术方案 §3.6）。
 *
 * <p>所有具体接入方式（Native / REST / Thrift / Phoenix）实现本接口； Gateway Server 只通过该接口访问 HBase，不直接引用任何具体实现。
 */
public interface HBaseAccessAdapter extends AutoCloseable {

  /** Adapter 名称，例如 {@code native-java}。 */
  String name();

  /** 能力声明，由 GetCapabilities 返回。 */
  AdapterCapabilities capabilities();

  /** 获取表结构。 */
  TableSchema getSchema(String namespace, String table) throws HBaseAccessException;

  /** 范围扫描，返回流式游标。 */
  ScanCursor scan(ScanRequest request) throws HBaseAccessException;

  /** 按 RowKey 批量读取，用于缓存部分命中时补列。 */
  ScanCursor batchGet(BatchGetRequest request) throws HBaseAccessException;

  /**
   * 取消指定请求。
   *
   * @return 是否确实取消了仍在执行的请求（幂等：已结束返回 false）
   */
  boolean cancel(String requestId);

  /** 健康状态。 */
  HealthStatus health();

  @Override
  void close() throws HBaseAccessException;
}
