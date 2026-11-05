#include "cache/cache_coverage_index.hpp"

#include <algorithm>

namespace duckdb {
namespace hbase {
namespace cache {

namespace {

//! start 侧比较：空字符串表示负无穷。
int CompareStart(const string &left, const string &right) {
    if (left.empty() && right.empty()) {
        return 0;
    }
    if (left.empty()) {
        return -1;
    }
    if (right.empty()) {
        return 1;
    }
    return CompareRowKey(left, right);
}

//! stop 侧比较：空字符串表示正无穷。
int CompareStop(const string &left, const string &right) {
    if (left.empty() && right.empty()) {
        return 0;
    }
    if (left.empty()) {
        return 1;
    }
    if (right.empty()) {
        return -1;
    }
    return CompareRowKey(left, right);
}

}  // namespace

bool RangeContains(const RowKeyRange &outer, const RowKeyRange &inner) {
    if (CompareStart(inner.start, outer.start) < 0) {
        return false;
    }
    if (CompareStop(inner.stop, outer.stop) > 0) {
        return false;
    }
    return true;
}

bool RangesIntersect(const RowKeyRange &left, const RowKeyRange &right) {
    // [a, b) 与 [c, d) 相交 <=> a < d 且 c < b
    if (!left.IsStopUnbounded() && CompareStart(right.start, left.stop) >= 0 &&
        !right.IsStartUnbounded()) {
        return false;
    }
    if (!right.IsStopUnbounded() && CompareStart(left.start, right.stop) >= 0 &&
        !left.IsStartUnbounded()) {
        return false;
    }
    return true;
}

CacheLookupResult ComputeCacheLookup(const vector<SegmentDescriptor> &segments, const ScanPlan &plan,
                                     int64_t now_ms) {
    CacheLookupResult result;
    result.status = CacheStatus::MISS;

    if (plan.required_columns.empty()) {
        result.reason = "no required columns";
        return result;
    }
    if (!plan.consistency.IsCacheable()) {
        result.reason = "scan consistency is not cacheable (max_versions>1 or timestamp range set)";
        return result;
    }
    // 空范围查询：结果必然为空，不需要读任何段
    if (plan.range.IsEmpty()) {
        result.status = CacheStatus::FULL_HIT;
        result.reason = "empty range";
        return result;
    }

    idx_t skipped_status = 0;
    idx_t skipped_expired = 0;
    idx_t skipped_consistency = 0;
    idx_t skipped_columns = 0;

    vector<const SegmentDescriptor *> candidates;
    for (auto &segment : segments) {
        if (segment.status != SegmentStatus::COMMITTED) {
            skipped_status++;
            continue;
        }
        if (segment.IsExpired(now_ms)) {
            skipped_expired++;
            continue;
        }
        if (!(segment.consistency == plan.consistency)) {
            skipped_consistency++;
            continue;
        }
        bool has_all_columns = true;
        for (auto &column : plan.required_columns) {
            if (!segment.HasColumn(column)) {
                has_all_columns = false;
                break;
            }
        }
        if (!has_all_columns) {
            skipped_columns++;
            continue;
        }
        // 保守：段范围必须落在请求范围内，否则读出的行会超出请求范围
        if (!RangeContains(plan.range, segment.range)) {
            continue;
        }
        candidates.push_back(&segment);
    }

    if (candidates.empty()) {
        result.reason = StringUtil::Format(
            "no usable segment (status=%llu, expired=%llu, consistency=%llu, columns=%llu)",
            static_cast<unsigned long long>(skipped_status),
            static_cast<unsigned long long>(skipped_expired),
            static_cast<unsigned long long>(skipped_consistency),
            static_cast<unsigned long long>(skipped_columns));
        return result;
    }

    // 起点相同时优先选更长的段，保证贪心结果确定
    std::sort(candidates.begin(), candidates.end(),
              [](const SegmentDescriptor *left, const SegmentDescriptor *right) {
                  auto cmp = CompareStart(left->range.start, right->range.start);
                  if (cmp != 0) {
                      return cmp < 0;
                  }
                  return CompareStop(left->range.stop, right->range.stop) > 0;
              });

    vector<const SegmentDescriptor *> selected;
    string cursor = plan.range.start;
    string failure_reason;

    while (true) {
        const SegmentDescriptor *best = nullptr;
        for (auto *candidate : candidates) {
            // 起点必须与游标精确重合：起点在游标之后会留缺口，
            // 起点在游标之前则会与已选段重叠（没有 RowKey 就无法裁剪重叠部分），
            // 两种情况都会导致行重复或缺失，宁可回源。
            if (CompareStart(candidate->range.start, cursor) != 0) {
                continue;
            }
            bool covers_cursor =
                candidate->range.stop.empty() || CompareRowKey(cursor, candidate->range.stop) < 0;
            if (!covers_cursor) {
                continue;
            }
            if (best == nullptr || CompareStop(candidate->range.stop, best->range.stop) > 0) {
                best = candidate;
            }
        }
        if (best == nullptr) {
            failure_reason = "gap in coverage at " +
                             (cursor.empty() ? string("<table-start>") : "<" + cursor + ">");
            break;
        }
        selected.push_back(best);
        if (best->range.stop.empty()) {
            // 段一直覆盖到表尾
            cursor = string();
            break;
        }
        cursor = best->range.stop;
        if (plan.range.stop.empty()) {
            // 请求无上界，但段有上界：还没铺满
            continue;
        }
        auto cmp = CompareRowKey(cursor, plan.range.stop);
        if (cmp == 0) {
            break;
        }
        if (cmp > 0) {
            failure_reason = "segment overlap beyond requested range";
            break;
        }
    }

    bool complete = false;
    if (failure_reason.empty()) {
        if (plan.range.stop.empty()) {
            complete = cursor.empty();
        } else {
            complete = !cursor.empty() && CompareRowKey(cursor, plan.range.stop) == 0;
        }
    }

    if (complete) {
        result.status = CacheStatus::FULL_HIT;
        for (auto *segment : selected) {
            result.segments.push_back(*segment);
        }
        return result;
    }

    // 未铺满：M3 一律回退 Cache Miss，并把缺口报给调用方用于诊断
    result.status = CacheStatus::MISS;
    result.reason = failure_reason.empty() ? "incomplete coverage" : failure_reason;
    RowKeyRange gap;
    gap.start = cursor;
    gap.stop = plan.range.stop;
    if (!(gap.start.empty() && gap.stop.empty())) {
        result.uncached_ranges.push_back(gap);
    }
    return result;
}

}  // namespace cache
}  // namespace hbase
}  // namespace duckdb
