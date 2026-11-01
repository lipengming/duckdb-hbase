package com.duckdb.hbase.spi;

import java.util.Objects;

/**
 * Adapter 能力声明，对应技术方案 §3.4 的 {@code ProviderCapabilities}。
 *
 * <p>Extension 在连接初始化时通过 {@code GetCapabilities} 获取， 不能假定所有适配器语义一致。
 */
public final class AdapterCapabilities {

  private final boolean supportsScan;
  private final boolean supportsBatchGet;
  private final boolean supportsRowKeyRange;
  private final boolean supportsColumnProjection;
  private final boolean supportsTimestamp;
  private final boolean supportsVersions;
  private final boolean supportsFilterPushdown;
  private final boolean supportsAsyncScan;
  private final boolean supportsCancellation;
  private final int maxBatchSize;
  private final int maxParallelScans;

  private AdapterCapabilities(Builder builder) {
    this.supportsScan = builder.supportsScan;
    this.supportsBatchGet = builder.supportsBatchGet;
    this.supportsRowKeyRange = builder.supportsRowKeyRange;
    this.supportsColumnProjection = builder.supportsColumnProjection;
    this.supportsTimestamp = builder.supportsTimestamp;
    this.supportsVersions = builder.supportsVersions;
    this.supportsFilterPushdown = builder.supportsFilterPushdown;
    this.supportsAsyncScan = builder.supportsAsyncScan;
    this.supportsCancellation = builder.supportsCancellation;
    this.maxBatchSize = builder.maxBatchSize;
    this.maxParallelScans = builder.maxParallelScans;
  }

  /** Native HBase Adapter 的默认能力：支持 Scan / BatchGet / 范围 / 投影 / 时间戳 / 版本 / 取消。 */
  public static AdapterCapabilities nativeDefaults() {
    return builder()
        .supportsScan(true)
        .supportsBatchGet(true)
        .supportsRowKeyRange(true)
        .supportsColumnProjection(true)
        .supportsTimestamp(true)
        .supportsVersions(true)
        .supportsFilterPushdown(false)
        .supportsAsyncScan(true)
        .supportsCancellation(true)
        .maxBatchSize(10_000)
        .maxParallelScans(8)
        .build();
  }

  public static Builder builder() {
    return new Builder();
  }

  public boolean supportsScan() {
    return supportsScan;
  }

  public boolean supportsBatchGet() {
    return supportsBatchGet;
  }

  public boolean supportsRowKeyRange() {
    return supportsRowKeyRange;
  }

  public boolean supportsColumnProjection() {
    return supportsColumnProjection;
  }

  public boolean supportsTimestamp() {
    return supportsTimestamp;
  }

  public boolean supportsVersions() {
    return supportsVersions;
  }

  public boolean supportsFilterPushdown() {
    return supportsFilterPushdown;
  }

  public boolean supportsAsyncScan() {
    return supportsAsyncScan;
  }

  public boolean supportsCancellation() {
    return supportsCancellation;
  }

  public int maxBatchSize() {
    return maxBatchSize;
  }

  public int maxParallelScans() {
    return maxParallelScans;
  }

  /** 能力构造器；未显式设置的布尔能力默认为 false，避免“默认支持”的隐性假设。 */
  public static final class Builder {
    private boolean supportsScan;
    private boolean supportsBatchGet;
    private boolean supportsRowKeyRange;
    private boolean supportsColumnProjection;
    private boolean supportsTimestamp;
    private boolean supportsVersions;
    private boolean supportsFilterPushdown;
    private boolean supportsAsyncScan;
    private boolean supportsCancellation;
    private int maxBatchSize = 1_000;
    private int maxParallelScans = 1;

    public Builder supportsScan(boolean value) {
      this.supportsScan = value;
      return this;
    }

    public Builder supportsBatchGet(boolean value) {
      this.supportsBatchGet = value;
      return this;
    }

    public Builder supportsRowKeyRange(boolean value) {
      this.supportsRowKeyRange = value;
      return this;
    }

    public Builder supportsColumnProjection(boolean value) {
      this.supportsColumnProjection = value;
      return this;
    }

    public Builder supportsTimestamp(boolean value) {
      this.supportsTimestamp = value;
      return this;
    }

    public Builder supportsVersions(boolean value) {
      this.supportsVersions = value;
      return this;
    }

    public Builder supportsFilterPushdown(boolean value) {
      this.supportsFilterPushdown = value;
      return this;
    }

    public Builder supportsAsyncScan(boolean value) {
      this.supportsAsyncScan = value;
      return this;
    }

    public Builder supportsCancellation(boolean value) {
      this.supportsCancellation = value;
      return this;
    }

    public Builder maxBatchSize(int value) {
      this.maxBatchSize = value;
      return this;
    }

    public Builder maxParallelScans(int value) {
      this.maxParallelScans = value;
      return this;
    }

    public AdapterCapabilities build() {
      if (maxBatchSize <= 0) {
        throw new IllegalArgumentException("maxBatchSize 必须为正数");
      }
      if (maxParallelScans <= 0) {
        throw new IllegalArgumentException("maxParallelScans 必须为正数");
      }
      return new AdapterCapabilities(this);
    }
  }

  @Override
  public String toString() {
    return "AdapterCapabilities{scan="
        + supportsScan
        + ", batchGet="
        + supportsBatchGet
        + ", maxBatchSize="
        + maxBatchSize
        + ", maxParallelScans="
        + maxParallelScans
        + '}';
  }

  @Override
  public boolean equals(Object other) {
    if (this == other) {
      return true;
    }
    if (!(other instanceof AdapterCapabilities)) {
      return false;
    }
    AdapterCapabilities that = (AdapterCapabilities) other;
    return supportsScan == that.supportsScan
        && supportsBatchGet == that.supportsBatchGet
        && supportsRowKeyRange == that.supportsRowKeyRange
        && supportsColumnProjection == that.supportsColumnProjection
        && supportsTimestamp == that.supportsTimestamp
        && supportsVersions == that.supportsVersions
        && supportsFilterPushdown == that.supportsFilterPushdown
        && supportsAsyncScan == that.supportsAsyncScan
        && supportsCancellation == that.supportsCancellation
        && maxBatchSize == that.maxBatchSize
        && maxParallelScans == that.maxParallelScans;
  }

  @Override
  public int hashCode() {
    return Objects.hash(
        supportsScan,
        supportsBatchGet,
        supportsRowKeyRange,
        supportsColumnProjection,
        supportsTimestamp,
        supportsVersions,
        supportsFilterPushdown,
        supportsAsyncScan,
        supportsCancellation,
        maxBatchSize,
        maxParallelScans);
  }
}
