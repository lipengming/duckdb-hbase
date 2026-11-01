package com.duckdb.hbase.core;

import com.duckdb.hbase.protocol.v1.BatchGetRequest;
import com.duckdb.hbase.protocol.v1.ScanRequest;
import com.duckdb.hbase.spi.AdapterCapabilities;
import com.duckdb.hbase.spi.HBaseAccessAdapter;
import com.duckdb.hbase.spi.HBaseAccessException;
import com.duckdb.hbase.spi.HealthStatus;
import com.duckdb.hbase.spi.ScanCursor;
import com.duckdb.hbase.spi.TableSchema;

/** 服务编排层：gRPC 实现只依赖本类，不直接接触 Adapter。 */
public final class GatewayService implements AutoCloseable {

  private final HBaseAccessAdapter adapter;

  public GatewayService(HBaseAccessAdapter adapter) {
    this.adapter = adapter;
  }

  public AdapterCapabilities capabilities() {
    return adapter.capabilities();
  }

  public TableSchema schema(String namespace, String table) throws HBaseAccessException {
    if (table == null || table.isEmpty()) {
      throw new HBaseAccessException("表名不能为空");
    }
    return adapter.getSchema(namespace, table);
  }

  public HealthStatus health() {
    return adapter.health();
  }

  public ScanCursor scan(ScanRequest request) throws HBaseAccessException {
    return adapter.scan(request);
  }

  public ScanCursor batchGet(BatchGetRequest request) throws HBaseAccessException {
    return adapter.batchGet(request);
  }

  public boolean cancel(String requestId) {
    return adapter.cancel(requestId);
  }

  @Override
  public void close() throws HBaseAccessException {
    adapter.close();
  }
}
