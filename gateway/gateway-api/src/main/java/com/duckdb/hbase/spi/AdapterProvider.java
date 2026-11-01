package com.duckdb.hbase.spi;

import java.util.Map;

/**
 * Adapter 提供者：通过 {@link java.util.ServiceLoader} 发现，Gateway 依赖倒置的关键。
 *
 * <p>实现类需在 {@code META-INF/services/com.duckdb.hbase.spi.AdapterProvider} 中登记。
 */
public interface AdapterProvider {

  /** 与配置项 {@code gateway.adapter} 对应的名称，例如 {@code native-java}。 */
  String name();

  /**
   * 创建 Adapter 实例。
   *
   * @param config 已去掉 {@code hbase.} 前缀的适配器配置
   */
  HBaseAccessAdapter create(Map<String, String> config) throws HBaseAccessException;
}
