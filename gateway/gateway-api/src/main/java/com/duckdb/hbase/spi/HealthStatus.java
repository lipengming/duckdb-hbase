package com.duckdb.hbase.spi;

/** Adapter 健康状态。{@code NOT_SERVING} 时 Gateway 应拒绝新请求但不退出进程。 */
public final class HealthStatus {

  public enum State {
    SERVING,
    NOT_SERVING,
    UNKNOWN
  }

  private final State state;
  private final String detail;

  private HealthStatus(State state, String detail) {
    this.state = state;
    this.detail = detail == null ? "" : detail;
  }

  public static HealthStatus serving() {
    return new HealthStatus(State.SERVING, "");
  }

  public static HealthStatus notServing(String detail) {
    return new HealthStatus(State.NOT_SERVING, detail);
  }

  public static HealthStatus unknown(String detail) {
    return new HealthStatus(State.UNKNOWN, detail);
  }

  public State state() {
    return state;
  }

  public String detail() {
    return detail;
  }

  @Override
  public String toString() {
    return "HealthStatus{" + state + (detail.isEmpty() ? "" : ", " + detail) + '}';
  }
}
