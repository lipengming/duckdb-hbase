package com.duckdb.hbase.core;

import static org.junit.jupiter.api.Assertions.assertThrows;

import com.duckdb.hbase.protocol.v1.ColumnRef;
import com.duckdb.hbase.spi.Row;
import java.io.IOException;
import java.util.Collections;
import org.junit.jupiter.api.Test;

class Utf8ColumnarEncoderTest {

  @Test
  void rejectsWholeFamilyProjectionInsteadOfSilentlyEncodingNulls() {
    ColumnRef family = ColumnRef.newBuilder().setFamily("cf1").setQualifier("").build();

    assertThrows(
        IOException.class,
        () ->
            new Utf8ColumnarEncoder()
                .encode(
                    Collections.singletonList(family),
                    Collections.singletonList(new Row(new byte[] {1}, Collections.emptyList()))));
  }
}
