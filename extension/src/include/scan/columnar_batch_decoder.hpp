#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace hbase {

//! 解码后的批次：按列保存 UTF-8 字符串与有效性位（M1 统一 VARCHAR）。
struct DecodedBatch {
    idx_t row_count = 0;
    vector<vector<string>> columns;
    vector<vector<bool>> valid;

    bool Empty() const { return row_count == 0; }
};

//! 解码 M1 列式载荷（Gateway 的 BATCH_ENCODING_UTF8_COLUMNS）。
//!
//! 格式（大端）：
//!   int32 magic('DHB1') | int32 columns | int32 rows
//!   每列：int32 nameLen + name(UTF8)
//!   每列：int32 length[rows]（-1=NULL）+ 拼接数据
//!
//! 失败抛 IOException，信息包含偏移量，便于定位协议不一致。
DecodedBatch DecodeColumnarBatch(const string& payload);

}  // namespace hbase
}  // namespace duckdb
