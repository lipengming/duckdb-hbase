package com.duckdb.hbase.core;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.Map;
import java.util.Properties;
import org.junit.jupiter.api.Test;

class GatewayConfigTest {

  @Test
  void adapterConfigStripsHbasePrefix() {
    Properties properties = new Properties();
    properties.setProperty("gateway.adapter", "native-java");
    properties.setProperty("hbase.zookeeper.quorum", "127.0.0.1");
    properties.setProperty("hbase.zookeeper.property.clientPort", "2181");

    GatewayConfig config = GatewayConfig.of(properties);
    Map<String, String> adapterConfig = config.adapterConfig();

    assertEquals("native-java", config.adapterName());
    assertEquals("127.0.0.1", adapterConfig.get("zookeeper.quorum"));
    assertEquals("2181", adapterConfig.get("zookeeper.property.clientPort"));
    assertEquals(2, adapterConfig.size());
  }

  @Test
  void defaultsAreStable() {
    GatewayConfig config = GatewayConfig.empty();

    assertEquals("native-java", config.adapterName());
    assertEquals("127.0.0.1", config.host());
    assertEquals(18080, config.port());
    assertTrue(config.adapterConfig().isEmpty());
  }

  @Test
  void invalidPortIsRejected() {
    Properties properties = new Properties();
    properties.setProperty("gateway.server.port", "70000");

    GatewayConfig config = GatewayConfig.of(properties);
    assertThrows(IllegalArgumentException.class, config::port);
  }
}
