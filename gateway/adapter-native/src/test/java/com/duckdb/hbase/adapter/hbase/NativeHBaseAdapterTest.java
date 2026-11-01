package com.duckdb.hbase.adapter.hbase;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.duckdb.hbase.protocol.v1.BatchGetRequest;
import com.duckdb.hbase.protocol.v1.ColumnRef;
import com.duckdb.hbase.protocol.v1.ScanRequest;
import com.duckdb.hbase.spi.HBaseAccessException;
import com.duckdb.hbase.spi.HealthStatus;
import com.duckdb.hbase.spi.Row;
import com.duckdb.hbase.spi.ScanCursor;
import com.duckdb.hbase.spi.TableSchema;
import com.google.protobuf.ByteString;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.Assumptions;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;

/**
 * Native Adapter 集成测试：需要本机 HBase 2.6.x 已启动、且已执行 scripts/hbase-fixture.sh。
 *
 * <p>HBase 不可达时用 JUnit Assumption 跳过，保证 CI（无 HBase）仍然全绿。
 */
class NativeHBaseAdapterTest {

  private static NativeHBaseAdapter adapter;
  private static boolean available;

  @BeforeAll
  static void setUp() {
    Map<String, String> config = new HashMap<>();
    config.put("zookeeper.quorum", System.getProperty("hbase.zookeeper.quorum", "127.0.0.1"));
    config.put(
        "zookeeper.property.clientPort",
        System.getProperty("hbase.zookeeper.property.clientPort", "2181"));
    try {
      adapter = new NativeHBaseAdapter(config);
      available = adapter.health().state() == HealthStatus.State.SERVING;
    } catch (HBaseAccessException e) {
      available = false;
    }
  }

  @AfterAll
  static void tearDown() throws HBaseAccessException {
    if (adapter != null && available) {
      adapter.close();
    }
  }

  @Test
  void capabilitiesCoverScanAndBatchGet() {
    Assumptions.assumeTrue(available, "本机未运行 HBase，跳过集成测试");

    assertTrue(adapter.capabilities().supportsScan());
    assertTrue(adapter.capabilities().supportsBatchGet());
    assertTrue(adapter.capabilities().supportsRowKeyRange());
    assertEquals("native-java", adapter.name());
  }

  @Test
  void schemaReturnsBothFamilies() throws HBaseAccessException {
    Assumptions.assumeTrue(available, "本机未运行 HBase，跳过集成测试");

    TableSchema schema = adapter.getSchema("duckdb_test", "customer_profile");
    List<String> families = new ArrayList<>();
    for (TableSchema.Column column : schema.columns()) {
      families.add(column.family());
    }
    assertTrue(families.contains("cf1"), "缺少列族 cf1: " + families);
    assertTrue(families.contains("cf2"), "缺少列族 cf2: " + families);
  }

  @Test
  void scanStreamsRowsInBatches() throws HBaseAccessException {
    Assumptions.assumeTrue(available, "本机未运行 HBase，跳过集成测试");

    ScanRequest request =
        ScanRequest.newBuilder()
            .setRequestId("it-scan-1")
            .setNamespace("duckdb_test")
            .setTable("customer_profile")
            .setStartRow(ByteString.copyFromUtf8("r001"))
            .setStopRow(ByteString.copyFromUtf8("r005"))
            .setIncludeStart(true)
            .setIncludeStop(false)
            .addColumns(ColumnRef.newBuilder().setFamily("cf1").setQualifier("name").build())
            .addColumns(ColumnRef.newBuilder().setFamily("cf2").setQualifier("age").build())
            .setMaxVersions(1)
            .setBatchSize(2)
            .build();

    List<Row> rows = new ArrayList<>();
    try (ScanCursor cursor = adapter.scan(request)) {
      List<Row> batch;
      while (!(batch = cursor.nextBatch(2)).isEmpty()) {
        rows.addAll(batch);
      }
    }

    assertEquals(4, rows.size(), "start 闭 / stop 开，r001..r004 共 4 行");
    assertEquals("r001", new String(rows.get(0).rowKey(), StandardCharsets.UTF_8));
    assertEquals("user-001", cellValue(rows.get(0), "cf1", "name"));
    assertEquals(2, rows.get(0).cells().size(), "请求的两列 cf1:name 与 cf2:age 都应返回");
  }

  @Test
  void batchGetFetchesRequestedRowKeys() throws HBaseAccessException {
    Assumptions.assumeTrue(available, "本机未运行 HBase，跳过集成测试");

    BatchGetRequest request =
        BatchGetRequest.newBuilder()
            .setRequestId("it-get-1")
            .setNamespace("duckdb_test")
            .setTable("customer_profile")
            .addRowkeys(ByteString.copyFromUtf8("r001"))
            .addRowkeys(ByteString.copyFromUtf8("r002"))
            .addRowkeys(ByteString.copyFromUtf8("r003"))
            .addColumns(ColumnRef.newBuilder().setFamily("cf2").setQualifier("score").build())
            .build();

    List<Row> rows = new ArrayList<>();
    try (ScanCursor cursor = adapter.batchGet(request)) {
      List<Row> batch;
      while (!(batch = cursor.nextBatch(10)).isEmpty()) {
        rows.addAll(batch);
      }
    }

    assertEquals(3, rows.size());
    assertEquals("3", cellValue(rows.get(0), "cf2", "score"), "r001 的 cf2:score = 1*3 % 100");
  }

  private static String cellValue(Row row, String family, String qualifier) {
    for (Row.Cell cell : row.cells()) {
      if (family.equals(new String(cell.family(), StandardCharsets.UTF_8))
          && qualifier.equals(new String(cell.qualifier(), StandardCharsets.UTF_8))) {
        return new String(cell.value(), StandardCharsets.UTF_8);
      }
    }
    return null;
  }
}
