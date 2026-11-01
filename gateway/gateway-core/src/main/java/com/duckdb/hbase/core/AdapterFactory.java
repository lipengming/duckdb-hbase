package com.duckdb.hbase.core;

import com.duckdb.hbase.spi.AdapterProvider;
import com.duckdb.hbase.spi.HBaseAccessAdapter;
import com.duckdb.hbase.spi.HBaseAccessException;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.ServiceLoader;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * 依据配置装配 Adapter（技术方案 §3.7）。
 *
 * <p>通过 {@link ServiceLoader} 发现实现，core 与 server 均不直接引用具体 Adapter， 包依赖方向保持 {@code adapter-* -> gateway-api}。
 */
public final class AdapterFactory {

  private static final Logger LOG = LoggerFactory.getLogger(AdapterFactory.class);

  private AdapterFactory() {}

  public static HBaseAccessAdapter create(GatewayConfig config) throws HBaseAccessException {
    String requested = config.adapterName();
    List<String> available = new ArrayList<>();
    Iterator<AdapterProvider> providers = ServiceLoader.load(AdapterProvider.class).iterator();
    while (providers.hasNext()) {
      AdapterProvider provider = providers.next();
      available.add(provider.name());
      if (provider.name().equals(requested)) {
        LOG.info("使用 adapter: {}", requested);
        return provider.create(config.adapterConfig());
      }
    }
    throw new HBaseAccessException(
        "未找到 adapter '" + requested + "'，当前 classpath 可用: " + available);
  }
}
