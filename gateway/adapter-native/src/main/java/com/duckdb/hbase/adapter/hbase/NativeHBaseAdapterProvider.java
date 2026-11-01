package com.duckdb.hbase.adapter.hbase;

import com.duckdb.hbase.spi.AdapterProvider;
import com.duckdb.hbase.spi.HBaseAccessAdapter;
import com.duckdb.hbase.spi.HBaseAccessException;
import java.util.Map;

/** 通过 ServiceLoader 暴露 Native HBase Adapter。 */
public final class NativeHBaseAdapterProvider implements AdapterProvider {

  @Override
  public String name() {
    return "native-java";
  }

  @Override
  public HBaseAccessAdapter create(Map<String, String> config) throws HBaseAccessException {
    return new NativeHBaseAdapter(config);
  }
}
