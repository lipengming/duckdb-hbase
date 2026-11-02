package com.duckdb.hbase.core;

import com.duckdb.hbase.protocol.v1.ErrorCode;
import com.duckdb.hbase.protocol.v1.ErrorInfo;
import io.grpc.Metadata;
import io.grpc.Status;
import io.grpc.StatusRuntimeException;

/**
 * Gateway 统一错误构造：把异常映射为 gRPC Status + {@link ErrorInfo} trailing metadata。
 *
 * <p>Extension 侧读取 metadata 还原错误码与可重试性；缺少 metadata 时退化为纯文本错误。
 */
public final class GatewayErrors {

  /** trailing metadata key（gRPC 约定：-bin 后缀表示二进制载荷）。 */
  public static final Metadata.Key<byte[]> ERROR_INFO_KEY =
      Metadata.Key.of("hbase-error-bin", Metadata.BINARY_BYTE_MARSHALLER);

  private GatewayErrors() {}

  public static StatusRuntimeException toException(
      ErrorCode code, String message, boolean retryable, String requestId) {
    ErrorInfo info =
        ErrorInfo.newBuilder()
            .setCode(code)
            .setMessage(message == null ? "" : message)
            .setRetryable(retryable)
            .setRequestId(requestId == null ? "" : requestId)
            .build();
    Metadata trailers = new Metadata();
    trailers.put(ERROR_INFO_KEY, info.toByteArray());
    return toStatus(code, message).asRuntimeException(trailers);
  }

  /**
   * 把 Adapter 抛出的异常粗分类为错误码：按类名判断，避免网关层直接依赖 HBase 客户端。
   */
  public static ErrorCode classify(Throwable error) {
    Throwable cursor = error;
    while (cursor != null) {
      String name = cursor.getClass().getSimpleName();
      if (name.contains("TableNotFound") || name.contains("NamespaceNotFound")) {
        return ErrorCode.ERROR_CODE_NOT_FOUND;
      }
      if (name.contains("AccessDenied") || name.contains("Authentication")) {
        return ErrorCode.ERROR_CODE_PERMISSION_DENIED;
      }
      if (cursor instanceof java.io.IOException) {
        return ErrorCode.ERROR_CODE_UNAVAILABLE;
      }
      cursor = cursor.getCause() == cursor ? null : cursor.getCause();
    }
    return ErrorCode.ERROR_CODE_INTERNAL;
  }

  /** 哪些错误适合重试：不可用 / 资源不足。 */
  public static boolean isRetryable(ErrorCode code) {
    return code == ErrorCode.ERROR_CODE_UNAVAILABLE
        || code == ErrorCode.ERROR_CODE_RESOURCE_EXHAUSTED;
  }

  /** 错误码 → gRPC Status，便于不使用 metadata 的客户端也能得到合理状态。 */
  public static Status toStatus(ErrorCode code, String message) {
    Status status;
    switch (code) {
      case ERROR_CODE_INVALID_REQUEST:
        status = Status.INVALID_ARGUMENT;
        break;
      case ERROR_CODE_NOT_FOUND:
        status = Status.NOT_FOUND;
        break;
      case ERROR_CODE_PERMISSION_DENIED:
        status = Status.PERMISSION_DENIED;
        break;
      case ERROR_CODE_UNAVAILABLE:
        status = Status.UNAVAILABLE;
        break;
      case ERROR_CODE_RESOURCE_EXHAUSTED:
        status = Status.RESOURCE_EXHAUSTED;
        break;
      case ERROR_CODE_CANCELLED:
        status = Status.CANCELLED;
        break;
      case ERROR_CODE_UNSUPPORTED:
        status = Status.UNIMPLEMENTED;
        break;
      default:
        status = Status.INTERNAL;
        break;
    }
    return message == null ? status : status.withDescription(message);
  }
}
