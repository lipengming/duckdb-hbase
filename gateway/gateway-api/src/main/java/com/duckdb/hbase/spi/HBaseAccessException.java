package com.duckdb.hbase.spi;

/**
 * Adapter 访问 HBase 失败时抛出的受检异常。
 *
 * <p>底层 HBase / HTTP / JDBC 异常必须转换为本异常并保留 cause， 避免把客户端实现细节泄露给 Gateway 上层。
 */
public class HBaseAccessException extends Exception {

  private static final long serialVersionUID = 1L;

  public HBaseAccessException(String message) {
    super(message);
  }

  public HBaseAccessException(String message, Throwable cause) {
    super(message, cause);
  }
}
