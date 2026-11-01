package com.duckdb.hbase.server;

import com.duckdb.hbase.core.GatewayService;
import com.duckdb.hbase.protocol.v1.BatchGetRequest;
import com.duckdb.hbase.protocol.v1.CancelRequest;
import com.duckdb.hbase.protocol.v1.CancelResponse;
import com.duckdb.hbase.protocol.v1.CapabilityRequest;
import com.duckdb.hbase.protocol.v1.CapabilityResponse;
import com.duckdb.hbase.protocol.v1.ColumnRef;
import com.duckdb.hbase.protocol.v1.ColumnSchema;
import com.duckdb.hbase.protocol.v1.HealthRequest;
import com.duckdb.hbase.protocol.v1.HealthResponse;
import com.duckdb.hbase.protocol.v1.HBaseDataServiceGrpc;
import com.duckdb.hbase.protocol.v1.ScanRequest;
import com.duckdb.hbase.protocol.v1.SchemaRequest;
import com.duckdb.hbase.protocol.v1.SchemaResponse;
import com.duckdb.hbase.protocol.v1.ArrowBatch;
import com.duckdb.hbase.spi.AdapterCapabilities;
import com.duckdb.hbase.spi.HBaseAccessException;
import com.duckdb.hbase.spi.HealthStatus;
import com.duckdb.hbase.spi.TableSchema;
import io.grpc.Status;
import io.grpc.stub.StreamObserver;
import java.util.Locale;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * gRPC 服务实现。
 *
 * <p>M1 状态：Health / GetCapabilities / GetSchema / Cancel 已接通 Adapter； Scan 与 BatchGet 需要 Arrow IPC 编码，尚未实现，返回 UNIMPLEMENTED。
 */
public final class HBaseDataServiceImpl extends HBaseDataServiceGrpc.HBaseDataServiceImplBase {

  private static final Logger LOG = LoggerFactory.getLogger(HBaseDataServiceImpl.class);
  private static final String VERSION = "0.1.0-SNAPSHOT";

  private final GatewayService service;

  public HBaseDataServiceImpl(GatewayService service) {
    this.service = service;
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
      responseObserver.onError(
          Status.INTERNAL.withDescription(e.getMessage()).withCause(e).asRuntimeException());
    }
  }

  @Override
  public void scan(ScanRequest request, StreamObserver<ArrowBatch> responseObserver) {
    responseObserver.onError(
        Status.UNIMPLEMENTED
            .withDescription("Scan 的 Arrow IPC 编码尚未实现（M1 进行中）")
            .asRuntimeException());
  }

  @Override
  public void batchGet(BatchGetRequest request, StreamObserver<ArrowBatch> responseObserver) {
    responseObserver.onError(
        Status.UNIMPLEMENTED
            .withDescription("BatchGet 的 Arrow IPC 编码尚未实现（M1 进行中）")
            .asRuntimeException());
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

  /** 方便后续列裁剪实现时复用：把协议中的 ColumnRef 转成可读字符串。 */
  static String formatColumn(ColumnRef column) {
    return column.getQualifier().isEmpty()
        ? column.getFamily()
        : column.getFamily() + ":" + column.getQualifier();
  }
}
