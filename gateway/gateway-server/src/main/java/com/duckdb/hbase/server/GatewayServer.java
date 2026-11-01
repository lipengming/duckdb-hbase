package com.duckdb.hbase.server;

import com.duckdb.hbase.core.AdapterFactory;
import com.duckdb.hbase.core.GatewayConfig;
import com.duckdb.hbase.core.GatewayService;
import com.duckdb.hbase.spi.HBaseAccessAdapter;
import com.duckdb.hbase.spi.HBaseAccessException;
import io.grpc.Server;
import io.grpc.ServerBuilder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.concurrent.TimeUnit;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * Gateway 进程入口。
 *
 * <p>用法：{@code java -jar gateway-server.jar [config.properties]}， 默认读取 {@code conf/application.properties}（不存在时使用内置默认值）。
 */
public final class GatewayServer {

  private static final Logger LOG = LoggerFactory.getLogger(GatewayServer.class);

  private GatewayServer() {}

  public static void main(String[] args) throws Exception {
    Path configPath =
        Paths.get(args.length > 0 ? args[0] : "conf/application.properties");

    GatewayConfig config;
    if (Files.exists(configPath)) {
      config = GatewayConfig.load(configPath);
      LOG.info("已加载配置: {}", configPath.toAbsolutePath());
    } else {
      config = GatewayConfig.empty();
      LOG.warn("未找到配置文件 {}，使用默认配置", configPath.toAbsolutePath());
    }

    HBaseAccessAdapter adapter = AdapterFactory.create(config);
    GatewayService service = new GatewayService(adapter);

    Server server =
        ServerBuilder.forPort(config.port())
            .addService(new HBaseDataServiceImpl(service))
            .build()
            .start();

    LOG.info(
        "DuckDB-HBase Gateway 已启动: {}:{} (adapter={})",
        config.host(),
        config.port(),
        adapter.name());

    Runtime.getRuntime()
        .addShutdownHook(
            new Thread(
                () -> {
                  LOG.info("正在关闭 Gateway...");
                  server.shutdown();
                  try {
                    if (!server.awaitTermination(10, TimeUnit.SECONDS)) {
                      server.shutdownNow();
                    }
                    service.close();
                  } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    server.shutdownNow();
                  } catch (HBaseAccessException e) {
                    LOG.warn("关闭 Adapter 失败", e);
                  }
                },
                "gateway-shutdown"));

    server.awaitTermination();
  }
}
