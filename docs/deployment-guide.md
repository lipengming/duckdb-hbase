# DuckDB-HBase 部署运维指南

> 提炼自 [技术方案 V1.0](../tech_v0.1.md) §4-§5、§8。若与方案冲突，以方案为准。

## 1. 部署拓扑

系统分为三层部署：DuckDB 进程（含 Extension 与本地缓存）、Java Gateway、HBase 集群。

| 层次 | 部署形态 | 说明 |
| --- | --- | --- |
| Extension | 随 DuckDB 进程加载 | 每个 DuckDB 进程独立加载 Extension，缓存默认属于单个进程 |
| Gateway | 独立 Java 进程与发行包 | 可单实例或多实例，通过 gRPC 提供 Scan、Get、Schema 与健康检查服务 |
| HBase | 既有集群 | Gateway 使用 HBase Java Client 2.x 访问 |

关键约束：

- Extension 与 Gateway 完全分离：Extension 不负责安装或启动 Gateway，两者独立发布、独立升级。
- Gateway 必须部署在能够访问 HBase 集群的网络环境中；生产环境应与 HBase 同网段，避免跨网段访问带来的额外延迟与不稳定。
- 多 Adapter 通过配置选择而非改代码：`gateway.adapter` 可取 `native-java`（默认）、`rest`、`thrift`、`phoenix`。从 Native Java 切换到 REST 时，Extension、Cache、Scan Planner 与 SQL 均不变，只更换 Gateway Adapter 和配置。
- 生产环境可部署多个 Gateway 实例，但需注意：Extension 本地 Cache 默认属于单个 DuckDB 进程，不同分析节点之间不自动共享 Cache；Gateway 实例之间需要共享连接配置和认证策略；远端 Scan 请求应携带 Request ID，便于取消和追踪。

## 2. Gateway 部署

### 2.1 发行包与启动

建议 Gateway 提供独立发行包：

```text
duckdb-hbase-gateway/
    bin/
        start.sh
        stop.sh
    conf/
        application.yml
    lib/
        gateway.jar
        hbase-client.jar
```

启动命令：

```bash
java -jar gateway.jar --spring.config.location=conf/application.yml
```

启动后做健康检查，确认进程可服务；生产环境可通过 systemd、Docker 或 Kubernetes 管理生命周期，具体 unit 文件、镜像与探针配置待定（TBD）：

```bash
curl http://127.0.0.1:18080/health
```

### 2.2 配置项

```yaml
gateway:
  adapter: native-java

  server:
    host: 127.0.0.1
    port: 18080

  hbase:
    quorum: zk1:2181,zk2:2181,zk3:2181
    znode_parent: /hbase
    rpc_timeout_ms: 30000

  pool:
    max_connections: 20

  security:
    mode: kerberos
    principal: hbase/_HOST@EXAMPLE.COM
    keytab: /etc/security/keytabs/hbase.keytab
```

| 配置项 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `gateway.adapter` | 枚举 | 待定（TBD，示例 `native-java`） | Adapter 选择：`native-java` / `rest` / `thrift` / `phoenix` |
| `gateway.server.host` | 字符串 | 待定（TBD，示例 `127.0.0.1`） | 监听地址，生产环境按网络规划配置 |
| `gateway.server.port` | 整数 | 待定（TBD，示例 `18080`） | gRPC 监听端口；具体默认值未定 |
| `gateway.hbase.quorum` | 字符串 | 无（必填） | ZooKeeper 地址列表，形如 `zk1:2181,zk2:2181` |
| `gateway.hbase.znode_parent` | 字符串 | 待定（TBD，示例 `/hbase`） | HBase 在 ZooKeeper 中的父节点 |
| `gateway.hbase.rpc_timeout_ms` | 整数 | 待定（TBD，示例 `30000`） | Gateway 访问 HBase 的 RPC 超时，单位毫秒 |
| `gateway.pool.max_connections` | 整数 | 待定（TBD，示例 `20`） | 连接池上限 |
| `gateway.security.mode` | 字符串 | 待定（TBD，示例 `kerberos`） | 安全模式，Kerberos 为生产必选能力 |
| `gateway.security.principal` | 字符串 | 待定（TBD，示例 `hbase/_HOST@EXAMPLE.COM`） | Kerberos 主体 |
| `gateway.security.keytab` | 字符串 | 待定（TBD，示例 `/etc/security/keytabs/hbase.keytab`） | keytab 路径，不得提交到仓库 |

线程池与工作线程数、日志级别与输出、TLS 证书与端口、请求限流阈值等配置项方案未定义，均为待定（TBD）。

## 3. 安全设计

必须支持六项能力：Kerberos 认证、TLS、Gateway API 认证、HBase Namespace / Table 权限控制、SQL 参数与配置的安全处理、审计日志。

- 传输加密：必须支持 TLS，客户端到 Gateway 的链路统一加密；正式协议需增加 TLS 与身份认证，具体证书管理方式待定（TBD）。
- 认证：生产环境使用 Kerberos；Gateway 对外还需独立的 API 认证机制，方案未指定具体形式（TBD）。
- 最小权限：按 Namespace / Table 粒度授权，Gateway 使用的 HBase 主体只应拥有所需表的读权限；第一阶段只读，不授予写权限。
- 输入安全：Gateway 不应接受客户端随意传入的 Java 类名、SQL 拼接片段或任意 HBase Filter 序列化对象；Phoenix SQL 必须采用参数绑定。
- 凭据管理：不要把密码写入 SQL 明文，应通过环境变量、Kerberos 或受保护的配置文件获取；keytab 不得进入版本库。
- 审计：必须记录审计日志并独立留存，字段与保留周期待定（TBD）。
- 限流：生产环境需支持请求限流与服务端资源限制，具体阈值待定（TBD）。

## 4. 超时与重试

超时按层级设置，任一层超时都会向上表现为查询失败或取消：

| 层级 | 配置/机制 | 说明 |
| --- | --- | --- |
| SQL → Extension | `hbase_connect` 的 `timeout_ms` | 客户端请求超时，方案示例 30000 毫秒，默认值 TBD |
| Extension → Gateway | 协议中的 Request Deadline | 正式实现需增加 Request Deadline，字段与默认值 TBD |
| Gateway → HBase | `gateway.hbase.rpc_timeout_ms` | 方案示例 30000 毫秒，默认值 TBD |
| HBase 内部 | 由 HBase Client 配置决定 | 方案未给出独立配置项（TBD） |

方案未给出各层推荐值（TBD），落地时需保证外层超时不短于内层，避免外层先超时导致内层资源悬挂。

重试策略按操作区分：

| 操作 | 策略 |
| --- | --- |
| Schema 查询 | 可有限重试 |
| Scan 建立 | 建立失败可以重试 |
| Scan 流读取 | 需谨慎，避免重复返回数据 |
| Batch Get | 按 RowKey 批次重试 |
| Cache 写入 | 允许 Abort 后重试 |
| Cancel | 幂等 |

幂等性约束：不能对一个已经返回部分结果的 Scan 直接从头重试，再把新数据拼接到旧数据中。查询取消时必须同时关闭远端 Scanner 和本地文件 Reader。若未来支持断点续传，需要引入最后处理的 RowKey 和稳定的扫描语义，当前版本不支持（TBD）。

## 5. 资源规划与容量估算

- Gateway 连接池：`gateway.pool.max_connections` 控制单实例连接数（示例 20），需与 HBase RegionServer 的连接承受能力匹配。
- 并发：Extension 每个查询可并行执行，并行度受 Gateway 能力协商中 `max_parallel_scans` 约束；单机并发查询数上限方案未给出（TBD）。
- 缓存内存：L1 为 Memory Cache，L2 为 Parquet，元数据使用 SQLite；内存占用与查询并发、批大小相关，具体估算公式待定（TBD）。
- 缓存磁盘：`hbase_cache_directory` 需容纳 `hbase_cache_max_size`（示例 `10GB`）、写入 staging 临时文件与 SQLite 元数据，并预留增长余量；建议磁盘可用空间显著大于 `hbase_cache_max_size`，具体比例待定（TBD）。
- 多节点：各分析节点缓存独立，容量需按节点数分别规划，不能假设共享。
- 结果集：全量结果禁止无界加载内存，必须流式消费，容量规划不应依赖单次查询返回全表。

## 6. 监控与日志

需要采集的指标（方案列出的性能指标即监控基线）：

| 指标 | 含义 |
| --- | --- |
| Query Latency | 查询延迟 |
| HBase Scan Count | 远端 Scan 次数 |
| HBase Get Count | 补列 Get 次数 |
| Cache Hit Rate | 缓存命中率 |
| Memory Usage | 内存占用 |
| Cache Write Throughput | 缓存写入吞吐 |
| Gateway Throughput | Gateway 吞吐 |

- 健康检查：Gateway 提供 `/health`，可接入负载均衡与编排系统的存活探针；响应体格式待定（TBD）。
- 请求追踪：远端请求携带 Request ID，正式实现还需 Trace ID 与 Request Deadline，便于跨 Extension 与 Gateway 关联排查。
- 日志：需记录错误码与错误详情，禁止在日志中输出凭据；日志级别、字段规范与保留周期待定（TBD）。
- 审计：独立于运行日志，记录访问主体与访问对象，字段待定（TBD）。
- 告警阈值：方案未定义，需结合真实压测结果设定（TBD）。

## 7. 升级与回滚

- Extension 与 Gateway 独立升级：两者通过 Provider 协议解耦，Gateway 升级不需要重新编译 Extension。
- Adapter 切换只改配置：`gateway.adapter` 变更并重启 Gateway 即可，Extension 与 SQL 不变；建议先在新实例验证再切流量。
- 版本兼容：Adapter SPI 应带版本号，避免 Gateway 实现与 Extension 协议不兼容；DuckDB C++ Extension API 不是稳定 ABI，升级 DuckDB 必须同步升级 Extension，并保持构建时锁定的 DuckDB 版本与源码 Commit 一致。
- 灰度：多 Gateway 实例场景下，可按分析节点切换 `endpoint` 到新实例实现灰度；灰度比例与回滚判定指标方案未定义（TBD）。
- 回滚：保留上一版 `gateway.jar`、`application.yml` 与 Extension 文件；回滚 Gateway 只需换回旧包并重启。
- 缓存兼容：缓存格式与元数据的跨版本兼容性未定义（TBD）。跨大版本升级或回滚前，建议先执行 `SELECT hbase_cache_clear();` 清理缓存，避免读到不兼容的 Segment。

## 8. 故障处置手册

| 故障 | 现象 | 处置 |
| --- | --- | --- |
| HBase 不可用 | Gateway 健康检查失败，未命中缓存的查询报错；完整命中的查询因不访问 HBase 仍可返回 | 确认 HBase 与 ZooKeeper 状态；Scan 建立失败可重试，已返回部分结果的 Scan 不得从头重试；恢复后先验证 `/health` 再放开流量 |
| Gateway OOM | 进程被 OOM Kill 或频繁 Full GC，查询超时增多 | 降低单查询批大小与并发，检查 `max_connections` 与实际负载；重启实例恢复；堆大小与内存参数待定（TBD）；长期依赖 Arrow 流式传输，禁止全量结果无界加载 |
| 缓存损坏 | 命中缓存后结果异常或查询报错 | 缓存写入在校验失败时 Abort；清理该表缓存或执行 `SELECT hbase_cache_clear();` 后重新查询；检查缓存目录下 staging 临时文件与 SQLite 元数据一致性 |
| 磁盘满 | 缓存写入失败，查询报错或失去缓存加速 | 清理缓存目录、下调 `hbase_cache_max_size`、扩容磁盘；监控 `hbase_cache_directory` 使用率并预留余量 |
| 查询被取消后资源未释放 | Gateway 连接或文件句柄持续增长 | 取消需同时关闭远端 Scanner 与本地 Reader；Cancel 必须幂等；必要时重启 Gateway 实例 |

## 9. 待定项清单

以下内容方案未给出结论，落地前需补齐：

1. Gateway 端口与监听地址的正式默认值（当前仅为示例 `18080` / `127.0.0.1`）。
2. 线程池、工作线程数与并发上限的配置项及默认值。
3. 日志级别、日志字段规范、日志保留周期。
4. TLS 的配置项、证书签发与轮换方式。
5. Gateway API 认证的具体机制。
6. 请求限流阈值与服务端资源限制。
7. 内存、连接数、磁盘容量的估算公式与推荐余量比例。
8. 各层超时的推荐值与 Request Deadline 字段定义。
9. 灰度发布的比例、判定指标与自动回滚条件。
10. 缓存格式与元数据的跨版本兼容策略。
11. 健康检查响应体格式与监控指标暴露方式（端点、格式、采集周期）。
12. 审计日志字段与留存要求。

## 10. 相关文档

- [总体架构](architecture.md)：分层与模块边界。
- [Gateway 设计](gateway-design.md)：Provider 协议、Adapter SPI 与能力协商。
- [缓存设计](cache-design.md)：缓存一致性、Segment 提交与失效。
- [用户使用指南](user-guide.md)：连接、查询与缓存管理 API。
- [测试与质量保障](testing.md)：集成测试覆盖的 Adapter 与故障场景。
