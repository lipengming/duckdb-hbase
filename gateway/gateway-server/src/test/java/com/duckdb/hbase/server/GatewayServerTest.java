package com.duckdb.hbase.server;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.duckdb.hbase.core.AdapterFactory;
import com.duckdb.hbase.core.GatewayConfig;
import com.duckdb.hbase.core.GatewayService;
import com.duckdb.hbase.protocol.v1.ArrowBatch;
import com.duckdb.hbase.protocol.v1.CapabilityRequest;
import com.duckdb.hbase.protocol.v1.CapabilityResponse;
import com.duckdb.hbase.protocol.v1.ColumnRef;
import com.duckdb.hbase.protocol.v1.HealthRequest;
import com.duckdb.hbase.protocol.v1.HealthResponse;
import com.duckdb.hbase.protocol.v1.HBaseDataServiceGrpc;
import com.duckdb.hbase.protocol.v1.ScanRequest;
import com.duckdb.hbase.protocol.v1.SchemaRequest;
import com.duckdb.hbase.protocol.v1.SchemaResponse;
import com.duckdb.hbase.spi.HBaseAccessAdapter;
import com.duckdb.hbase.spi.HBaseAccessException;
import com.duckdb.hbase.spi.HealthStatus;
import com.google.protobuf.ByteString;
import io.grpc.ManagedChannel;
import io.grpc.ManagedChannelBuilder;
import io.grpc.Server;
import io.grpc.ServerBuilder;
import java.util.Iterator;
import java.util.Properties;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.Assumptions;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;

/**
 * gRPC 层端到端测试：真实 Gateway Server + 真实 HBase。
 *
 * <p>需要本机 HBase 已启动并执行过 scripts/hbase-fixture.sh；不可达时整体跳过。
 */
class GatewayServerTest {

  private static HBaseAccessAdapter adapter;
  private static GatewayService service;
  private static Server server;
  private static ManagedChannel channel;
  private static HBaseDataServiceGrpc.HBaseDataServiceBlockingStub stub;
  private static boolean available;

  @BeforeAll
  static void setUp() throws Exception {
    Properties properties = new Properties();
    properties.setProperty("gateway.adapter", "native-java");
    properties.setProperty("hbase.zookeeper.quorum", "127.0.0.1");
    properties.setProperty("hbase.zookeeper.property.clientPort", "2181");

    try {
      adapter = AdapterFactory.create(GatewayConfig.of(properties));
      available = adapter.health().state() == HealthStatus.State.SERVING;
    } catch (HBaseAccessException | RuntimeException e) {
      available = false;
    }
    Assumptions.assumeTrue(available, "本机未运行 HBase，跳过 Gateway 集成测试");

    service = new GatewayService(adapter);
    server = ServerBuilder.forPort(0).addService(new HBaseDataServiceImpl(service)).build().start();
    channel =
        ManagedChannelBuilder.forAddress("127.0.0.1", server.getPort()).usePlaintext().build();
    stub = HBaseDataServiceGrpc.newBlockingStub(channel);
  }

  @AfterAll
  static void tearDown() throws Exception {
    if (channel != null) {
      channel.shutdownNow().awaitTermination(5, TimeUnit.SECONDS);
    }
    if (server != null) {
      server.shutdownNow().awaitTermination(5, TimeUnit.SECONDS);
    }
    if (service != null) {
      service.close();
    }
  }

  @Test
  void healthReturnsServing() {
    HealthResponse response = stub.health(HealthRequest.getDefaultInstance());
    assertEquals("serving", response.getStatus());
    assertTrue(response.getVersion().contains("0.1.0"));
  }

  @Test
  void capabilitiesExposeScanAndBatchGet() {
    CapabilityResponse response = stub.getCapabilities(CapabilityRequest.getDefaultInstance());
    assertTrue(response.getSupportsScan());
    assertTrue(response.getSupportsBatchGet());
    assertTrue(response.getMaxBatchSize() > 0);
  }

  @Test
  void schemaListsColumnFamilies() {
    SchemaResponse response =
        stub.getSchema(
            SchemaRequest.newBuilder().setNamespace("duckdb_test").setTable("customer_profile").build());
    assertTrue(
        response.getColumnsList().stream().anyMatch(column -> column.getFamily().equals("cf1")));
    assertTrue(
        response.getColumnsList().stream().anyMatch(column -> column.getFamily().equals("cf2")));
  }

  @Test
  void scanStreamsArrowBatches() {
    Iterator<ArrowBatch> batches =
        stub.scan(
            ScanRequest.newBuilder()
                .setNamespace("duckdb_test")
                .setTable("customer_profile")
                .setStartRow(ByteString.copyFromUtf8("r001"))
                .setStopRow(ByteString.copyFromUtf8("r011"))
                .setIncludeStart(true)
                .addColumns(ColumnRef.newBuilder().setFamily("cf1").setQualifier("name").build())
                .setBatchSize(4)
                .build());

    long rows = 0;
    int payloads = 0;
    while (batches.hasNext()) {
      ArrowBatch batch = batches.next();
      if (batch.getFinished()) {
        break;
      }
      assertTrue(batch.getRowCount() > 0);
      assertTrue(batch.getIpcPayload().size() > 0);
      rows += batch.getRowCount();
      payloads++;
    }
    assertEquals(10, rows, "r001..r010 共 10 行");
    assertTrue(payloads >= 2, "batchSize=4 时至少应分 3 批");
  }
}
