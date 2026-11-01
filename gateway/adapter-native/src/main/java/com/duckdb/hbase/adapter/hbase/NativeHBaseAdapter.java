package com.duckdb.hbase.adapter.hbase;

import com.duckdb.hbase.protocol.v1.BatchGetRequest;
import com.duckdb.hbase.protocol.v1.ColumnRef;
import com.duckdb.hbase.protocol.v1.ScanRequest;
import com.duckdb.hbase.spi.AdapterCapabilities;
import com.duckdb.hbase.spi.HBaseAccessAdapter;
import com.duckdb.hbase.spi.HBaseAccessException;
import com.duckdb.hbase.spi.HealthStatus;
import com.duckdb.hbase.spi.ScanCursor;
import com.duckdb.hbase.spi.TableSchema;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.hbase.HBaseConfiguration;
import org.apache.hadoop.hbase.TableName;
import org.apache.hadoop.hbase.client.Admin;
import org.apache.hadoop.hbase.client.ColumnFamilyDescriptor;
import org.apache.hadoop.hbase.client.Connection;
import org.apache.hadoop.hbase.client.ConnectionFactory;
import org.apache.hadoop.hbase.client.Get;
import org.apache.hadoop.hbase.client.Result;
import org.apache.hadoop.hbase.client.ResultScanner;
import org.apache.hadoop.hbase.client.Scan;
import org.apache.hadoop.hbase.client.Table;
import org.apache.hadoop.hbase.client.TableDescriptor;
import org.apache.hadoop.hbase.util.Bytes;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * 基于 HBase Java Client 2.x 的原生 Adapter（默认实现）。
 *
 * <p>注意：Java 关键字 {@code native} 不能作为包名片段， 因此包名为 {@code com.duckdb.hbase.adapter.hbase}（与文档中的 {@code adapter.native} 命名约定略有差异， 已在
 * docs/gateway-design.md 记录）。
 */
public final class NativeHBaseAdapter implements HBaseAccessAdapter {

  private static final Logger LOG = LoggerFactory.getLogger(NativeHBaseAdapter.class);

  private final Connection connection;
  private final ConcurrentMap<String, ScanCursor> activeCursors = new ConcurrentHashMap<>();

  public NativeHBaseAdapter(Map<String, String> config) throws HBaseAccessException {
    Configuration configuration = HBaseConfiguration.create();
    // 配置来自 application.properties 的 hbase.* 段，装配时已去掉前缀
    for (Map.Entry<String, String> entry : config.entrySet()) {
      configuration.set("hbase." + entry.getKey(), entry.getValue());
    }
    try {
      this.connection = ConnectionFactory.createConnection(configuration);
      LOG.info("Native HBase Adapter 已连接，配置项 {}", config.keySet());
    } catch (IOException e) {
      throw new HBaseAccessException("创建 HBase 连接失败: " + e.getMessage(), e);
    }
  }

  @Override
  public String name() {
    return "native-java";
  }

  @Override
  public AdapterCapabilities capabilities() {
    return AdapterCapabilities.nativeDefaults();
  }

  @Override
  public TableSchema getSchema(String namespace, String table) throws HBaseAccessException {
    TableName tableName = toTableName(namespace, table);
    try (Admin admin = connection.getAdmin()) {
      TableDescriptor descriptor = admin.getDescriptor(tableName);
      List<TableSchema.Column> columns = new ArrayList<>();
      for (ColumnFamilyDescriptor family : descriptor.getColumnFamilies()) {
        // HBase 是 schemaless：Descriptor 只有列族，没有 qualifier，具体列需在读取时发现
        columns.add(new TableSchema.Column(family.getNameAsString(), "", "VARCHAR"));
      }
      return new TableSchema(namespace, table, columns);
    } catch (IOException e) {
      throw new HBaseAccessException("获取表结构失败: " + tableName.getNameAsString(), e);
    }
  }

  @Override
  public ScanCursor scan(ScanRequest request) throws HBaseAccessException {
    TableName tableName = toTableName(request.getNamespace(), request.getTable());
    Scan scan = new Scan();

    if (!request.getStartRow().isEmpty()) {
      scan.withStartRow(request.getStartRow().toByteArray(), request.getIncludeStart());
    }
    if (!request.getStopRow().isEmpty()) {
      scan.withStopRow(request.getStopRow().toByteArray(), request.getIncludeStop());
    }
    applyColumns(scan, request.getColumnsList());

    scan.readVersions(Math.max(1, request.getMaxVersions()));
    if (request.getMaxTimestamp() > 0) {
      scan.setTimeRange(request.getMinTimestamp(), request.getMaxTimestamp());
    }
    if (request.getBatchSize() > 0) {
      scan.setCaching(request.getBatchSize());
    }

    Table table = null;
    try {
      table = connection.getTable(tableName);
      ResultScanner scanner = table.getScanner(scan);
      NativeScanCursor cursor = new NativeScanCursor(request.getRequestId(), table, scanner);
      register(cursor);
      return cursor;
    } catch (IOException e) {
      closeQuietly(table);
      throw new HBaseAccessException("启动 Scan 失败: " + tableName.getNameAsString(), e);
    }
  }

  @Override
  public ScanCursor batchGet(BatchGetRequest request) throws HBaseAccessException {
    TableName tableName = toTableName(request.getNamespace(), request.getTable());
    List<Get> gets = new ArrayList<>(request.getRowkeysCount());
    for (com.google.protobuf.ByteString rowKey : request.getRowkeysList()) {
      Get get = new Get(rowKey.toByteArray());
      applyColumns(get, request.getColumnsList());
      if (request.getMaxTimestamp() > 0) {
        get.setTimeRange(request.getMinTimestamp(), request.getMaxTimestamp());
      }
      gets.add(get);
    }

    Table table = null;
    try {
      table = connection.getTable(tableName);
      Result[] results = table.get(gets);
      NativeGetCursor cursor = new NativeGetCursor(request.getRequestId(), table, results);
      register(cursor);
      return cursor;
    } catch (IOException e) {
      closeQuietly(table);
      throw new HBaseAccessException("BatchGet 失败: " + tableName.getNameAsString(), e);
    }
  }

  @Override
  public boolean cancel(String requestId) {
    ScanCursor cursor = activeCursors.remove(requestId);
    if (cursor == null) {
      return false;
    }
    closeQuietly(cursor);
    return true;
  }

  @Override
  public HealthStatus health() {
    if (connection == null || connection.isClosed()) {
      return HealthStatus.notServing("HBase 连接已关闭");
    }
    return HealthStatus.serving();
  }

  @Override
  public void close() throws HBaseAccessException {
    for (ScanCursor cursor : activeCursors.values()) {
      closeQuietly(cursor);
    }
    activeCursors.clear();
    try {
      connection.close();
    } catch (IOException e) {
      throw new HBaseAccessException("关闭 HBase 连接失败", e);
    }
  }

  private void register(ScanCursor cursor) {
    if (cursor.requestId() != null && !cursor.requestId().isEmpty()) {
      activeCursors.put(cursor.requestId(), cursor);
    }
  }

  private static void applyColumns(Scan scan, List<ColumnRef> columns) {
    for (ColumnRef column : columns) {
      if (column.getQualifier().isEmpty()) {
        scan.addFamily(Bytes.toBytes(column.getFamily()));
      } else {
        scan.addColumn(Bytes.toBytes(column.getFamily()), Bytes.toBytes(column.getQualifier()));
      }
    }
  }

  private static void applyColumns(Get get, List<ColumnRef> columns) {
    for (ColumnRef column : columns) {
      if (column.getQualifier().isEmpty()) {
        get.addFamily(Bytes.toBytes(column.getFamily()));
      } else {
        get.addColumn(Bytes.toBytes(column.getFamily()), Bytes.toBytes(column.getQualifier()));
      }
    }
  }

  private static TableName toTableName(String namespace, String table) {
    if (namespace == null || namespace.isEmpty()) {
      return TableName.valueOf(table);
    }
    return TableName.valueOf(namespace, table);
  }

  private static void closeQuietly(Table table) {
    if (table == null) {
      return;
    }
    try {
      table.close();
    } catch (IOException e) {
      LOG.warn("关闭 Table 失败", e);
    }
  }

  private static void closeQuietly(ScanCursor cursor) {
    try {
      cursor.close();
    } catch (HBaseAccessException e) {
      LOG.warn("关闭游标失败", e);
    }
  }
}
