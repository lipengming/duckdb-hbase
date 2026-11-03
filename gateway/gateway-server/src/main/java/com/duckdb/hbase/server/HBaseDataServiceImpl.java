package com.duckdb.hbase.server;

import com.duckdb.hbase.core.Utf8ColumnarEncoder;
import com.duckdb.hbase.core.GatewayErrors;
import com.duckdb.hbase.core.GatewayService;
import com.duckdb.hbase.protocol.v1.ErrorCode;
import com.duckdb.hbase.protocol.v1.ArrowBatch;
import com.duckdb.hbase.protocol.v1.BatchEncoding;
import com.duckdb.hbase.protocol.v1.BatchGetRequest;
import com.duckdb.hbase.protocol.v1.CancelRequest;
import com.duckdb.hbase.protocol.v1.CancelResponse;
import com.duckdb.hbase.protocol.v1.CapabilityRequest;
import com.duckdb.hbase.protocol.v1.CapabilityResponse;
import com.duckdb.hbase.protocol.v1.ColumnRef;
import com.duckdb.hbase.protocol.v1.ColumnSchema;
import com.duckdb.hbase.protocol.v1.HBaseDataServiceGrpc;
import com.duckdb.hbase.protocol.v1.HealthRequest;
import com.duckdb.hbase.protocol.v1.HealthResponse;
import com.duckdb.hbase.protocol.v1.ScanRequest;
import com.duckdb.hbase.protocol.v1.SchemaRequest;
import com.duckdb.hbase.protocol.v1.SchemaResponse;
import com.duckdb.hbase.spi.AdapterCapabilities;
import com.duckdb.hbase.spi.HBaseAccessException;
import com.duckdb.hbase.spi.HealthStatus;
import com.duckdb.hbase.spi.Row;
import com.duckdb.hbase.spi.ScanCursor;
import com.duckdb.hbase.spi.TableSchema;
import com.google.protobuf.ByteString;
import io.grpc.Status;
import io.grpc.stub.StreamObserver;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * gRPC 服务实现。
 *
 * <p>数据面：Scan / BatchGet 分批评审，每批编码为一条独立的 Arrow IPC stream 放入 {@code ArrowBatch.ipc_payload}， 客户端逐批解码即可，不依赖跨批状态。
 */
public final class HBaseDataServiceImpl extends HBaseDataServiceGrpc.HBaseDataServiceImplBase {

  private static final Logger LOG = LoggerFactory.getLogger(HBaseDataServiceImpl.class);
  private static final String VERSION = "0.1.0-SNAPSHOT";
  private static final int DEFAULT_BATCH_SIZE = 4096;
  private static final int MAX_BATCH_SIZE = 65536;

  private final GatewayService service;
  private final Utf8ColumnarEncoder encoder;

  public HBaseDataServiceImpl(GatewayService service) {
    this(service, new Utf8ColumnarEncoder());
  }

  public HBaseDataServiceImpl(GatewayService service, Utf8ColumnarEncoder encoder) {
    this.service = service;
    this.encoder = encoder;
  }

  /** 预留：编码器若持有资源，可在此释放。 */
  public void close() {
    // 当前编码器无持有资源
  }

  @Override
  public void getCapabilities(
      CapabilityRequest request, StreamObserver<CapabilityResponse> responseObserver) {
    AdapterCapabilities capabilities = service.capabilities();
    CapabilityResponse response =
        CapabilityResponse.newBuilder()
            .setSupportsScan(capabilities.supportsScan())
            .setSupportsBatchGet(capabilities.supportsBatchGet())
            .setSupportsRowkeyRange(capabilities.supportsRowKeyRange())
            .setSupportsColumnProjection(capabilities.supportsColumnProjection())
            .setSupportsTimestamp(capabilities.supportsTimestamp())
            .setSupportsVersions(capabilities.supportsVersions())
            .setSupportsFilterPushdown(capabilities.supportsFilterPushdown())
            .setSupportsAsyncScan(capabilities.supportsAsyncScan())
            .setSupportsCancellation(capabilities.supportsCancellation())
            .setMaxBatchSize(capabilities.maxBatchSize())
            .setMaxParallelScans(capabilities.maxParallelScans())
            .build();
    responseObserver.onNext(response);
    responseObserver.onCompleted();
  }

  @Override
  public void getSchema(SchemaRequest request, StreamObserver<SchemaResponse> responseObserver) {
    try {
      TableSchema schema = service.schema(request.getNamespace(), request.getTable());
      SchemaResponse.Builder builder = SchemaResponse.newBuilder();
      for (TableSchema.Column column : schema.columns()) {
        builder.addColumns(
            ColumnSchema.newBuilder()
                .setFamily(column.family())
                .setQualifier(column.qualifier())
                .setSqlType(column.sqlType())
                .build());
      }
      responseObserver.onNext(builder.build());
      responseObserver.onCompleted();
    } catch (HBaseAccessException e) {
      LOG.warn("GetSchema 失败: {}.{}", request.getNamespace(), request.getTable(), e);
      ErrorCode code = GatewayErrors.classify(e);
      responseObserver.onError(
          GatewayErrors.toException(code, e.getMessage(), GatewayErrors.isRetryable(code), ""));
    }
  }

  @Override
  public void scan(ScanRequest request, StreamObserver<ArrowBatch> responseObserver) {
    try (ScanCursor cursor = service.scan(request)) {
      List<ColumnRef> columns =
          resolveColumns(request.getNamespace(), request.getTable(), request.getColumnsList());
      stream(cursor, columns, request.getBatchSize(), request.getMaxVersions() > 1, responseObserver);
    } catch (HBaseAccessException e) {
      LOG.warn("Scan 失败: {}.{}", request.getNamespace(), request.getTable(), e);
      ErrorCode code = GatewayErrors.classify(e);
      responseObserver.onError(
          GatewayErrors.toException(
              code, e.getMessage(), GatewayErrors.isRetryable(code), request.getRequestId()));
    }
  }

  @Override
  public void batchGet(BatchGetRequest request, StreamObserver<ArrowBatch> responseObserver) {
    try (ScanCursor cursor = service.batchGet(request)) {
      List<ColumnRef> columns =
          resolveColumns(request.getNamespace(), request.getTable(), request.getColumnsList());
      stream(cursor, columns, 0, false, responseObserver);
    } catch (HBaseAccessException e) {
      LOG.warn("BatchGet 失败: {}.{}", request.getNamespace(), request.getTable(), e);
      ErrorCode code = GatewayErrors.classify(e);
      responseObserver.onError(
          GatewayErrors.toException(
              code, e.getMessage(), GatewayErrors.isRetryable(code), request.getRequestId()));
    }
  }

  @Override
  public void cancel(CancelRequest request, StreamObserver<CancelResponse> responseObserver) {
    boolean cancelled = service.cancel(request.getRequestId());
    responseObserver.onNext(CancelResponse.newBuilder().setCancelled(cancelled).build());
    responseObserver.onCompleted();
  }

  @Override
  public void health(HealthRequest request, StreamObserver<HealthResponse> responseObserver) {
    HealthStatus health = service.health();
    HealthResponse response =
        HealthResponse.newBuilder()
            .setStatus(health.state().name().toLowerCase(Locale.ROOT))
            .setVersion(VERSION)
            .build();
    responseObserver.onNext(response);
    responseObserver.onCompleted();
  }

  /** 逐批拉取 → 编码 → 推送；空批次表示结束，最后补一个 finished 标记。 */
  private void stream(
      ScanCursor cursor, List<ColumnRef> columns, int requestedBatchSize, boolean expandVersions,
      StreamObserver<ArrowBatch> responseObserver)
      throws HBaseAccessException {
    int batchSize = requestedBatchSize > 0 ? Math.min(requestedBatchSize, MAX_BATCH_SIZE) : DEFAULT_BATCH_SIZE;
    long sequence = 0;
    try {
      while (true) {
        List<Row> rows = cursor.nextBatch(batchSize);
        if (rows.isEmpty()) {
          break;
        }
        Utf8ColumnarEncoder.EncodedBatch encoded = encoder.encode(columns, rows, expandVersions);
        responseObserver.onNext(
            ArrowBatch.newBuilder()
                .setIpcPayload(ByteString.copyFrom(encoded.payload()))
                .setSequenceNumber(sequence++)
                .setRowCount(encoded.rowCount())
                .setEncoding(BatchEncoding.BATCH_ENCODING_UTF8_COLUMNS)
                .setFinished(false)
                .build());
      }
      responseObserver.onNext(
          ArrowBatch.newBuilder().setSequenceNumber(sequence).setFinished(true).build());
      responseObserver.onCompleted();
    } catch (IOException e) {
      throw new HBaseAccessException("Arrow IPC 编码失败: " + e.getMessage(), e);
    }
  }

  /**
   * 请求未指定列时，退化为整列族（GetSchema 只能拿到列族，qualifier 需在读取时发现）。
   */
  private List<ColumnRef> resolveColumns(String namespace, String table, List<ColumnRef> requested)
      throws HBaseAccessException {
    if (!requested.isEmpty()) {
      return requested;
    }
    TableSchema schema = service.schema(namespace, table);
    List<ColumnRef> columns = new ArrayList<>();
    for (TableSchema.Column column : schema.columns()) {
      columns.add(ColumnRef.newBuilder().setFamily(column.family()).build());
    }
    if (columns.isEmpty()) {
      throw new HBaseAccessException("表 " + namespace + "." + table + " 没有可用列，请显式指定 columns");
    }
    return columns;
  }

  /** 方便后续列裁剪实现时复用：把协议中的 ColumnRef 转成可读字符串。 */
  static String formatColumn(ColumnRef column) {
    return column.getQualifier().isEmpty()
        ? column.getFamily()
        : column.getFamily() + ":" + column.getQualifier();
  }
}
