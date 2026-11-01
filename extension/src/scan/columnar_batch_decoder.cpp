#include "scan/columnar_batch_decoder.hpp"

#include "duckdb/common/exception.hpp"

#include <cstring>

namespace duckdb {
namespace hbase {

namespace {

constexpr uint32_t kMagic = 0x44484231;  // "DHB1"

//! 大端读取器；越界统一抛 IOException。
class PayloadReader {
public:
    explicit PayloadReader(const string& payload) : data_(payload), offset_(0) {}

    uint32_t ReadUInt32() {
        Require(4);
        uint32_t value = (static_cast<uint8_t>(data_[offset_]) << 24) |
                         (static_cast<uint8_t>(data_[offset_ + 1]) << 16) |
                         (static_cast<uint8_t>(data_[offset_ + 2]) << 8) |
                         static_cast<uint8_t>(data_[offset_ + 3]);
        offset_ += 4;
        return value;
    }

    int32_t ReadInt32() { return static_cast<int32_t>(ReadUInt32()); }

    string ReadBytes(idx_t length) {
        Require(length);
        string result = data_.substr(offset_, length);
        offset_ += length;
        return result;
    }

    void Skip(idx_t length) {
        Require(length);
        offset_ += length;
    }

    bool Done() const { return offset_ >= data_.size(); }

private:
    void Require(idx_t length) const {
        if (offset_ + length > data_.size()) {
            throw IOException("列式载荷越界：offset=%llu, need=%llu, size=%llu",
                              static_cast<unsigned long long>(offset_),
                              static_cast<unsigned long long>(length),
                              static_cast<unsigned long long>(data_.size()));
        }
    }

    const string& data_;
    idx_t offset_;
};

}  // namespace

DecodedBatch DecodeColumnarBatch(const string& payload) {
    if (payload.size() < 4) {
        throw IOException("列式载荷过短：%llu 字节",
                          static_cast<unsigned long long>(payload.size()));
    }

    PayloadReader reader(payload);
    auto magic = reader.ReadUInt32();
    if (magic != kMagic) {
        throw IOException("列式载荷 magic 不匹配：0x%08x（期望 0x%08x）", magic, kMagic);
    }

    auto column_count = reader.ReadInt32();
    auto row_count = reader.ReadInt32();
    if (column_count < 0 || row_count < 0) {
        throw IOException("列式载荷头部非法：columns=%d, rows=%d", column_count, row_count);
    }

    DecodedBatch decoded;
    decoded.row_count = static_cast<idx_t>(row_count);
    decoded.columns.resize(static_cast<idx_t>(column_count));
    decoded.valid.resize(static_cast<idx_t>(column_count));

    // 列名（当前解码按位置对应 Bind 列，名称仅用于校验长度）
    vector<string> names;
    names.reserve(static_cast<idx_t>(column_count));
    for (int32_t i = 0; i < column_count; i++) {
        auto name_length = reader.ReadInt32();
        if (name_length < 0) {
            throw IOException("列名长度非法：%d", name_length);
        }
        names.push_back(reader.ReadBytes(static_cast<idx_t>(name_length)));
    }

    for (int32_t column_index = 0; column_index < column_count; column_index++) {
        auto& values = decoded.columns[static_cast<idx_t>(column_index)];
        auto& valid = decoded.valid[static_cast<idx_t>(column_index)];
        values.resize(decoded.row_count);
        valid.resize(decoded.row_count);

        vector<int32_t> lengths(decoded.row_count);
        for (idx_t row = 0; row < decoded.row_count; row++) {
            lengths[row] = reader.ReadInt32();
            if (lengths[row] < -1) {
                throw IOException("列 %s 第 %llu 行长度非法：%d",
                                  names[static_cast<idx_t>(column_index)],
                                  static_cast<unsigned long long>(row), lengths[row]);
            }
        }
        for (idx_t row = 0; row < decoded.row_count; row++) {
            if (lengths[row] < 0) {
                valid[row] = false;
            } else {
                valid[row] = true;
                values[row] = reader.ReadBytes(static_cast<idx_t>(lengths[row]));
            }
        }
    }

    return decoded;
}

}  // namespace hbase
}  // namespace duckdb
