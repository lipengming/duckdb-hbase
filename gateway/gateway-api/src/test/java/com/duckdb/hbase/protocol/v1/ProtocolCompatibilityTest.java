package com.duckdb.hbase.protocol.v1;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

/**
 * 协议兼容性守卫：字段编号一旦发布不得变更/复用。
 *
 * <p>若有人调整字段编号，这里会先失败，避免线上新旧客户端不兼容。
 */
class ProtocolCompatibilityTest {

  @Test
  void arrowBatchFieldNumbersAreStable() {
    assertEquals("ipc_payload", ArrowBatch.getDescriptor().findFieldByNumber(1).getName());
    assertEquals("sequence_number", ArrowBatch.getDescriptor().findFieldByNumber(2).getName());
    assertEquals("finished", ArrowBatch.getDescriptor().findFieldByNumber(3).getName());
    assertEquals("row_count", ArrowBatch.getDescriptor().findFieldByNumber(4).getName());
    assertEquals("encoding", ArrowBatch.getDescriptor().findFieldByNumber(5).getName());
  }

  @Test
  void scanRequestFieldNumbersAreStable() {
    assertEquals("request_id", ScanRequest.getDescriptor().findFieldByNumber(1).getName());
    assertEquals("namespace", ScanRequest.getDescriptor().findFieldByNumber(2).getName());
    assertEquals("start_row", ScanRequest.getDescriptor().findFieldByNumber(4).getName());
    assertEquals("stop_row", ScanRequest.getDescriptor().findFieldByNumber(5).getName());
    assertEquals("columns", ScanRequest.getDescriptor().findFieldByNumber(8).getName());
    assertEquals("batch_size", ScanRequest.getDescriptor().findFieldByNumber(12).getName());
  }

  @Test
  void errorInfoFieldNumbersAreStable() {
    assertEquals("code", ErrorInfo.getDescriptor().findFieldByNumber(1).getName());
    assertEquals("message", ErrorInfo.getDescriptor().findFieldByNumber(2).getName());
    assertEquals("retryable", ErrorInfo.getDescriptor().findFieldByNumber(3).getName());
    assertEquals("request_id", ErrorInfo.getDescriptor().findFieldByNumber(4).getName());
  }

  @Test
  void capabilityResponseCoversElevenCapabilities() {
    assertEquals(11, CapabilityResponse.getDescriptor().getFields().size());
  }
}
