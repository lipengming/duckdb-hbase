#pragma once

#include "cache/cache_types.hpp"

namespace duckdb {
namespace hbase {
namespace cache {

//! 判断 inner 是否完全落在 outer 之内（半开区间，空 RowKey 表示该侧无界）。
bool RangeContains(const RowKeyRange &outer, const RowKeyRange &inner);

//! 两个半开区间是否相交（用于诊断与统计）。
bool RangesIntersect(const RowKeyRange &left, const RowKeyRange &right);

//! 求解覆盖计划：从候选段中挑选一组互不重叠、按 RowKey 升序、正好铺满请求范围的段。
//!
//! 纯函数，不读任何 IO，便于单测。判定规则（M3）：
//!   1. status 必须是 COMMITTED；
//!   2. 不得过期（expires_at == 0 表示不过期）；
//!   3. ScanConsistency 必须与请求完全一致；
//!   4. 段列集合必须包含全部 RequiredColumns（按 family/qualifier/logical_type 比较）；
//!   5. 段范围必须是请求范围的子集（保守：段比请求大时读出的行会越界，直接不参与覆盖）；
//!   6. 段必须能无缺口地拼接出请求范围，任何缺口一律 MISS（M3 不做 Partial Hit）。
//!
//! 硬约束：范围比较必须使用 HBase 字节序（CompareRowKey），禁止字符串字典序。
CacheLookupResult ComputeCacheLookup(const vector<SegmentDescriptor> &segments, const ScanPlan &plan,
                                     int64_t now_ms);

}  // namespace cache
}  // namespace hbase
}  // namespace duckdb
