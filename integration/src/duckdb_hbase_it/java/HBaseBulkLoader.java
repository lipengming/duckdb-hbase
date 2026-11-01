/*
 * DuckDB-HBase 集成测试夹具装载器。
 *
 * 为什么不用 hbase shell 的 put：单客户 100 万行 × 15 列 = 1500 万 Cell，
 * 逐行 shell put 需要数小时；这里用 HBase 2.x BufferedMutator 批量写入，
 * 1M 行实测在分钟级完成。
 *
 * 子命令（统一用 --key=value 传参）：
 *   --mode=ping    检查表是否存在与预分区数量，输出 JSON
 *   --mode=load    建表（可选预分区）+ 从 TSV(.gz) 批量装载，输出 JSON 统计
 *   --mode=count   按 RowKey 范围计数（FirstKeyOnlyFilter，不取列值），输出 JSON
 *   --mode=scan    按 RowKey 范围或指定 rowkeys 取行，输出 TSV（首行为表头）
 *
 * 约定：
 *   - TSV 首列固定为 rowkey，其余列名为 family:qualifier；
 *   - 空值占位符 \N 表示「不写该 Cell」（读回为 NULL）；
 *   - scan 模式对不存在的 Cell 同样输出 \N，保证列位置固定；
 *   - 进度与错误写 stderr，结构化结果写 stdout。
 */

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.HashMap;
import java.util.zip.GZIPInputStream;

import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.hbase.HBaseConfiguration;
import org.apache.hadoop.hbase.NamespaceDescriptor;
import org.apache.hadoop.hbase.NamespaceNotFoundException;
import org.apache.hadoop.hbase.TableName;
import org.apache.hadoop.hbase.client.Admin;
import org.apache.hadoop.hbase.client.BufferedMutator;
import org.apache.hadoop.hbase.client.BufferedMutatorParams;
import org.apache.hadoop.hbase.client.ColumnFamilyDescriptorBuilder;
import org.apache.hadoop.hbase.client.Connection;
import org.apache.hadoop.hbase.client.ConnectionFactory;
import org.apache.hadoop.hbase.client.Get;
import org.apache.hadoop.hbase.client.Put;
import org.apache.hadoop.hbase.client.Result;
import org.apache.hadoop.hbase.client.ResultScanner;
import org.apache.hadoop.hbase.client.Scan;
import org.apache.hadoop.hbase.client.Table;
import org.apache.hadoop.hbase.client.TableDescriptorBuilder;
import org.apache.hadoop.hbase.filter.FirstKeyOnlyFilter;
import org.apache.hadoop.hbase.util.Bytes;

public final class HBaseBulkLoader {

    private static final String NULL_TOKEN = "\\N";
    private static final int DEFAULT_WRITE_BUFFER = 8 * 1024 * 1024;

    private HBaseBulkLoader() {
    }

    public static void main(String[] args) {
        try {
            Map<String, String> opts = parseArgs(args);
            String mode = opts.getOrDefault("mode", "");
            if (mode.isEmpty()) {
                throw new IllegalArgumentException("缺少 --mode（ping/load/count/scan）");
            }
            switch (mode) {
                case "ping":
                    ping(opts);
                    break;
                case "load":
                    load(opts);
                    break;
                case "count":
                    count(opts);
                    break;
                case "scan":
                    scan(opts);
                    break;
                default:
                    throw new IllegalArgumentException("未知 mode: " + mode);
            }
        } catch (Throwable failure) {
            System.err.println("HBaseBulkLoader ERROR: " + failure);
            failure.printStackTrace(System.err);
            System.out.println("{\"error\":" + json(failure.toString()) + "}");
            System.out.flush();
            System.exit(2);
        }
    }

    // ------------------------------------------------------------------
    // 参数
    // ------------------------------------------------------------------

    private static Map<String, String> parseArgs(String[] args) {
        Map<String, String> opts = new HashMap<>();
        for (String arg : args) {
            if (!arg.startsWith("--")) {
                throw new IllegalArgumentException("不支持的参数: " + arg);
            }
            String body = arg.substring(2);
            int equals = body.indexOf('=');
            if (equals < 0) {
                opts.put(body, "true");
            } else {
                opts.put(body.substring(0, equals), body.substring(equals + 1));
            }
        }
        return opts;
    }

    private static String required(Map<String, String> opts, String key) {
        String value = opts.get(key);
        if (value == null || value.isEmpty()) {
            throw new IllegalArgumentException("缺少 --" + key);
        }
        return value;
    }

    private static String opt(Map<String, String> opts, String key, String fallback) {
        String value = opts.get(key);
        return value == null ? fallback : value;
    }

    private static boolean flag(Map<String, String> opts, String key) {
        String value = opts.get(key);
        return value != null && (value.equals("true") || value.isEmpty());
    }

    private static Configuration configuration() {
        return HBaseConfiguration.create();
    }

    // ------------------------------------------------------------------
    // ping
    // ------------------------------------------------------------------

    private static void ping(Map<String, String> opts) throws Exception {
        String tableName = required(opts, "table");
        TableName table = TableName.valueOf(tableName);
        try (Connection connection = ConnectionFactory.createConnection(configuration());
             Admin admin = connection.getAdmin()) {
            boolean exists = admin.tableExists(table);
            int regions = 0;
            if (exists) {
                regions = connection.getRegionLocator(table).getStartKeys().length;
            }
            StringBuilder out = new StringBuilder();
            out.append("{\"mode\":\"ping\",\"table\":").append(json(tableName))
               .append(",\"exists\":").append(exists)
               .append(",\"regions\":").append(regions).append('}');
            System.out.println(out);
        }
    }

    // ------------------------------------------------------------------
    // load
    // ------------------------------------------------------------------

    private static void load(Map<String, String> opts) throws Exception {
        String tableName = required(opts, "table");
        String inputPath = required(opts, "input");
        String[] families = required(opts, "families").split(",");
        boolean recreate = flag(opts, "recreate");
        boolean append = flag(opts, "append");
        int progressEvery = Integer.parseInt(opt(opts, "progress-every", "100000"));
        String[] splitKeys = opt(opts, "splits", "").isEmpty()
                ? new String[0] : opt(opts, "splits", "").split(",");

        TableName table = TableName.valueOf(tableName);
        Configuration conf = configuration();
        long started = System.currentTimeMillis();
        long rows = 0;
        long cells = 0;

        try (Connection connection = ConnectionFactory.createConnection(conf);
             Admin admin = connection.getAdmin()) {
            ensureNamespace(admin, table);
            if (admin.tableExists(table)) {
                if (recreate) {
                    disableAndDrop(admin, table);
                } else if (!append) {
                    throw new IllegalStateException(
                            "表已存在: " + tableName + "；使用 --recreate 重建或 --append 追加");
                }
            }
            if (!admin.tableExists(table)) {
                createTable(admin, table, families, splitKeys);
                System.err.println("已创建表 " + tableName + "（" + families.length + " 个列族，"
                        + (splitKeys.length + 1) + " 个 Region）");
            }

            BufferedMutatorParams params = new BufferedMutatorParams(table)
                    .writeBufferSize(DEFAULT_WRITE_BUFFER);
            try (BufferedMutator mutator = connection.getBufferedMutator(params)) {
                try (BufferedReader reader = openReader(inputPath)) {
                    String headerLine = reader.readLine();
                    if (headerLine == null) {
                        throw new IllegalArgumentException("输入文件为空: " + inputPath);
                    }
                    String[] columns = headerLine.split("\t", -1);
                    if (columns.length < 2 || !columns[0].equals("rowkey")) {
                        throw new IllegalArgumentException("表头非法，首列必须是 rowkey");
                    }
                    byte[][] familyBytes = new byte[columns.length][];
                    byte[][] qualifierBytes = new byte[columns.length][];
                    for (int index = 1; index < columns.length; index++) {
                        int separator = columns[index].indexOf(':');
                        if (separator <= 0 || separator == columns[index].length() - 1) {
                            throw new IllegalArgumentException(
                                    "列名非法（需要 family:qualifier）: " + columns[index]);
                        }
                        familyBytes[index] = Bytes.toBytes(columns[index].substring(0, separator));
                        qualifierBytes[index] = Bytes.toBytes(columns[index].substring(separator + 1));
                    }

                    String line;
                    long lineNumber = 1;
                    while ((line = reader.readLine()) != null) {
                        lineNumber++;
                        String[] fields = line.split("\t", -1);
                        if (fields.length != columns.length) {
                            throw new IllegalArgumentException("第 " + lineNumber + " 行列数为 "
                                    + fields.length + "，期望 " + columns.length);
                        }
                        Put put = new Put(Bytes.toBytes(fields[0]));
                        int added = 0;
                        for (int index = 1; index < columns.length; index++) {
                            if (NULL_TOKEN.equals(fields[index])) {
                                continue;
                            }
                            put.addColumn(familyBytes[index], qualifierBytes[index],
                                    Bytes.toBytes(fields[index]));
                            added++;
                        }
                        if (added == 0) {
                            continue;
                        }
                        mutator.mutate(put);
                        rows++;
                        cells += added;
                        if (progressEvery > 0 && rows % progressEvery == 0) {
                            long elapsed = Math.max(System.currentTimeMillis() - started, 1);
                            System.err.printf("  装载中 %,d 行（%,d 行/秒）%n",
                                    rows, rows * 1000 / elapsed);
                        }
                    }
                }
                mutator.flush();
            }
        }

        long elapsed = System.currentTimeMillis() - started;
        System.out.println("{\"mode\":\"load\",\"table\":" + json(tableName)
                + ",\"rows\":" + rows
                + ",\"cells\":" + cells
                + ",\"regions\":" + (splitKeys.length + 1)
                + ",\"ms\":" + elapsed + "}");
        System.err.println("装载完成：" + rows + " 行，" + cells + " Cell，耗时 " + elapsed + " ms");
    }

    private static void ensureNamespace(Admin admin, TableName table) throws Exception {
        String namespace = table.getNamespaceAsString();
        if (namespace == null || namespace.isEmpty()
                || namespace.equals(NamespaceDescriptor.DEFAULT_NAMESPACE_NAME_STR)) {
            return;
        }
        try {
            admin.getNamespaceDescriptor(namespace);
        } catch (NamespaceNotFoundException missing) {
            admin.createNamespace(NamespaceDescriptor.create(namespace).build());
            System.err.println("已创建命名空间 " + namespace);
        }
    }

    private static void disableAndDrop(Admin admin, TableName table) throws Exception {
        if (admin.isTableEnabled(table)) {
            admin.disableTable(table);
        }
        admin.deleteTable(table);
        System.err.println("已删除旧表 " + table);
    }

    private static void createTable(Admin admin, TableName table, String[] families,
                                    String[] splitKeys) throws Exception {
        TableDescriptorBuilder builder = TableDescriptorBuilder.newBuilder(table);
        for (String family : families) {
            builder.setColumnFamily(ColumnFamilyDescriptorBuilder.newBuilder(
                    Bytes.toBytes(family)).setMaxVersions(1).build());
        }
        if (splitKeys.length == 0) {
            admin.createTable(builder.build());
        } else {
            byte[][] splits = new byte[splitKeys.length][];
            for (int index = 0; index < splitKeys.length; index++) {
                splits[index] = Bytes.toBytes(splitKeys[index]);
            }
            admin.createTable(builder.build(), splits);
        }
    }

    private static BufferedReader openReader(String path) throws Exception {
        InputStream raw = new FileInputStream(path);
        InputStream stream = path.endsWith(".gz") ? new GZIPInputStream(raw, 1 << 16) : raw;
        return new BufferedReader(new InputStreamReader(stream, StandardCharsets.UTF_8), 1 << 20);
    }

    // ------------------------------------------------------------------
    // count
    // ------------------------------------------------------------------

    private static void count(Map<String, String> opts) throws Exception {
        String tableName = required(opts, "table");
        TableName table = TableName.valueOf(tableName);
        long started = System.currentTimeMillis();
        long count = 0;

        Scan scan = new Scan();
        applyRange(scan, opts);
        scan.setCaching(1000);
        scan.setFilter(new FirstKeyOnlyFilter());

        try (Connection connection = ConnectionFactory.createConnection(configuration());
             Table htable = connection.getTable(table);
             ResultScanner scanner = htable.getScanner(scan)) {
            for (Result ignored : scanner) {
                count++;
            }
        }
        long elapsed = System.currentTimeMillis() - started;
        System.out.println("{\"mode\":\"count\",\"table\":" + json(tableName)
                + ",\"count\":" + count + ",\"ms\":" + elapsed + "}");
    }

    // ------------------------------------------------------------------
    // scan
    // ------------------------------------------------------------------

    private static void scan(Map<String, String> opts) throws Exception {
        String tableName = required(opts, "table");
        String columnSpec = required(opts, "columns");
        String[] columns = columnSpec.split(",");
        int limit = Integer.parseInt(opt(opts, "limit", "100"));
        String rowkeys = opt(opts, "rowkeys", "");
        TableName table = TableName.valueOf(tableName);

        byte[][] families = new byte[columns.length][];
        byte[][] qualifiers = new byte[columns.length][];
        for (int index = 0; index < columns.length; index++) {
            int separator = columns[index].indexOf(':');
            if (separator <= 0 || separator == columns[index].length() - 1) {
                throw new IllegalArgumentException("列名非法: " + columns[index]);
            }
            families[index] = Bytes.toBytes(columns[index].substring(0, separator));
            qualifiers[index] = Bytes.toBytes(columns[index].substring(separator + 1));
        }

        BufferedWriter writer = new BufferedWriter(
                new OutputStreamWriter(new FileOutputStream(java.io.FileDescriptor.out),
                        StandardCharsets.UTF_8), 1 << 16);
        StringBuilder header = new StringBuilder("rowkey");
        for (String column : columns) {
            header.append('\t').append(column);
        }
        writer.write(header.toString());
        writer.write('\n');

        try (Connection connection = ConnectionFactory.createConnection(configuration());
             Table htable = connection.getTable(table)) {
            if (!rowkeys.isEmpty()) {
                List<Get> gets = new ArrayList<>();
                for (String rowkey : rowkeys.split(",")) {
                    if (!rowkey.isEmpty()) {
                        gets.add(new Get(Bytes.toBytes(rowkey)));
                    }
                }
                for (Result result : htable.get(gets)) {
                    if (result == null || result.isEmpty()) {
                        continue;
                    }
                    writeRow(writer, result, families, qualifiers);
                }
            } else {
                Scan scan = new Scan();
                applyRange(scan, opts);
                scan.setCaching(500);
                int written = 0;
                try (ResultScanner scanner = htable.getScanner(scan)) {
                    for (Result result : scanner) {
                        writeRow(writer, result, families, qualifiers);
                        written++;
                        if (written >= limit) {
                            break;
                        }
                    }
                }
            }
        }
        writer.flush();
    }

    private static void writeRow(BufferedWriter writer, Result result,
                                 byte[][] families, byte[][] qualifiers) throws Exception {
        writer.write(Bytes.toString(result.getRow()));
        for (int index = 0; index < families.length; index++) {
            byte[] value = result.getValue(families[index], qualifiers[index]);
            writer.write('\t');
            writer.write(value == null ? NULL_TOKEN : Bytes.toString(value));
        }
        writer.write('\n');
    }

    private static void applyRange(Scan scan, Map<String, String> opts) {
        String start = opt(opts, "start", "");
        String stop = opt(opts, "stop", "");
        if (!start.isEmpty()) {
            scan.withStartRow(Bytes.toBytes(start), true);
        }
        if (!stop.isEmpty()) {
            scan.withStopRow(Bytes.toBytes(stop), false);
        }
    }

    // ------------------------------------------------------------------
    // 工具
    // ------------------------------------------------------------------

    private static String json(String value) {
        StringBuilder out = new StringBuilder("\"");
        for (int index = 0; index < value.length(); index++) {
            char ch = value.charAt(index);
            switch (ch) {
                case '"':
                    out.append("\\\"");
                    break;
                case '\\':
                    out.append("\\\\");
                    break;
                case '\n':
                    out.append("\\n");
                    break;
                case '\r':
                    out.append("\\r");
                    break;
                case '\t':
                    out.append("\\t");
                    break;
                default:
                    if (ch < 0x20) {
                        out.append(String.format("\\u%04x", (int) ch));
                    } else {
                        out.append(ch);
                    }
            }
        }
        return out.append('"').toString();
    }
}
