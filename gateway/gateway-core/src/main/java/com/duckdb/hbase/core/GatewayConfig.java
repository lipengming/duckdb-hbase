package com.duckdb.hbase.core;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Properties;

/**
 * Gateway 配置。
 *
 * <p>M1 使用 {@code .properties}（JDK 自带、零依赖）；YAML 配置见 docs/deployment-guide.md， 待 M2 引入配置库后再切换，避免现在为此增加依赖。
 *
 * <p>约定：{@code gateway.*} 为服务自身配置，{@code hbase.*} 为透传给 Adapter 的配置。
 */
public final class GatewayConfig {

  private final Properties properties;

  private GatewayConfig(Properties properties) {
    this.properties = properties;
  }

  public static GatewayConfig empty() {
    return new GatewayConfig(new Properties());
  }

  public static GatewayConfig of(Properties properties) {
    Properties copy = new Properties();
    copy.putAll(properties);
    return new GatewayConfig(copy);
  }

  public static GatewayConfig load(Path path) throws IOException {
    Properties properties = new Properties();
    try (InputStream in = Files.newInputStream(path)) {
      properties.load(in);
    }
    return of(properties);
  }

  public String adapterName() {
    return get("gateway.adapter", "native-java");
  }

  public String host() {
    return get("gateway.server.host", "127.0.0.1");
  }

  public int port() {
    String raw = get("gateway.server.port", "18080");
    try {
      int port = Integer.parseInt(raw.trim());
      if (port < 1 || port > 65535) {
        throw new IllegalArgumentException("端口超出范围: " + raw);
      }
      return port;
    } catch (NumberFormatException e) {
      throw new IllegalArgumentException("gateway.server.port 不是合法整数: " + raw, e);
    }
  }

  /** Adapter 配置：去掉 {@code hbase.} 前缀后的键值。 */
  public Map<String, String> adapterConfig() {
    Map<String, String> result = new LinkedHashMap<>();
    for (String name : properties.stringPropertyNames()) {
      if (name.startsWith("hbase.")) {
        result.put(name.substring("hbase.".length()), properties.getProperty(name));
      }
    }
    return Collections.unmodifiableMap(result);
  }

  public String get(String key, String defaultValue) {
    String value = properties.getProperty(key);
    return value == null ? defaultValue : value.trim();
  }
}
