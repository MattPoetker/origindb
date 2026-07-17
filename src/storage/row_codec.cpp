#include "storage/row_codec.h"

#include <cstring>
#include <stdexcept>

namespace origindb {

namespace {

// ---- little-endian append helpers ----

void PutU8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

void PutU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; i++) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void PutBytes(std::vector<uint8_t>& out, const void* p, size_t n) {
    PutU32(out, static_cast<uint32_t>(n));
    const auto* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
}

void PutString(std::vector<uint8_t>& out, const std::string& s) {
    PutBytes(out, s.data(), s.size());
}

// ---- bounded little-endian reader ----

class Reader {
public:
    Reader(const uint8_t* data, size_t len) : p_(data), end_(data + len) {}

    uint8_t U8() {
        Need(1);
        return *p_++;
    }
    uint32_t U32() {
        Need(4);
        uint32_t v = static_cast<uint32_t>(p_[0]) |
                     (static_cast<uint32_t>(p_[1]) << 8) |
                     (static_cast<uint32_t>(p_[2]) << 16) |
                     (static_cast<uint32_t>(p_[3]) << 24);
        p_ += 4;
        return v;
    }
    uint64_t U64() {
        Need(8);
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(p_[i]) << (8 * i);
        p_ += 8;
        return v;
    }
    std::string Str() {
        uint32_t n = U32();
        Need(n);
        std::string s(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return s;
    }
    std::vector<uint8_t> Bytes() {
        uint32_t n = U32();
        Need(n);
        std::vector<uint8_t> b(p_, p_ + n);
        p_ += n;
        return b;
    }
    template <typename T>
    T Fixed() {
        Need(sizeof(T));
        T v;
        std::memcpy(&v, p_, sizeof(T));
        p_ += sizeof(T);
        return v;
    }

private:
    void Need(size_t n) const {
        if (static_cast<size_t>(end_ - p_) < n) {
            throw std::runtime_error("row_codec: truncated record");
        }
    }
    const uint8_t* p_;
    const uint8_t* end_;
};

// type tags — keep in sync with the Value variant order
enum : uint8_t {
    TAG_NULL = 0,
    TAG_INT32 = 1,
    TAG_INT64 = 2,
    TAG_FLOAT = 3,
    TAG_DOUBLE = 4,
    TAG_STRING = 5,
    TAG_BYTES = 6,
    TAG_BOOL = 7,
    TAG_TIMESTAMP = 8,
};

void EncodeValue(std::vector<uint8_t>& out, const Value& value) {
    std::visit(
        [&out](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                PutU8(out, TAG_NULL);
            } else if constexpr (std::is_same_v<T, int32_t>) {
                PutU8(out, TAG_INT32);
                out.insert(out.end(), reinterpret_cast<const uint8_t*>(&v),
                           reinterpret_cast<const uint8_t*>(&v) + sizeof(v));
            } else if constexpr (std::is_same_v<T, int64_t>) {
                PutU8(out, TAG_INT64);
                out.insert(out.end(), reinterpret_cast<const uint8_t*>(&v),
                           reinterpret_cast<const uint8_t*>(&v) + sizeof(v));
            } else if constexpr (std::is_same_v<T, float>) {
                PutU8(out, TAG_FLOAT);
                out.insert(out.end(), reinterpret_cast<const uint8_t*>(&v),
                           reinterpret_cast<const uint8_t*>(&v) + sizeof(v));
            } else if constexpr (std::is_same_v<T, double>) {
                PutU8(out, TAG_DOUBLE);
                out.insert(out.end(), reinterpret_cast<const uint8_t*>(&v),
                           reinterpret_cast<const uint8_t*>(&v) + sizeof(v));
            } else if constexpr (std::is_same_v<T, std::string>) {
                PutU8(out, TAG_STRING);
                PutString(out, v);
            } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
                PutU8(out, TAG_BYTES);
                PutBytes(out, v.data(), v.size());
            } else if constexpr (std::is_same_v<T, bool>) {
                PutU8(out, TAG_BOOL);
                PutU8(out, v ? 1 : 0);
            } else if constexpr (std::is_same_v<
                                     T, std::chrono::system_clock::time_point>) {
                PutU8(out, TAG_TIMESTAMP);
                int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 v.time_since_epoch())
                                 .count();
                PutU64(out, static_cast<uint64_t>(ns));
            }
        },
        value);
}

Value DecodeValue(Reader& r) {
    uint8_t tag = r.U8();
    switch (tag) {
        case TAG_NULL:
            return std::monostate{};
        case TAG_INT32:
            return r.Fixed<int32_t>();
        case TAG_INT64:
            return r.Fixed<int64_t>();
        case TAG_FLOAT:
            return r.Fixed<float>();
        case TAG_DOUBLE:
            return r.Fixed<double>();
        case TAG_STRING:
            return r.Str();
        case TAG_BYTES:
            return r.Bytes();
        case TAG_BOOL:
            return static_cast<bool>(r.U8());
        case TAG_TIMESTAMP: {
            int64_t ns = static_cast<int64_t>(r.U64());
            return std::chrono::system_clock::time_point(
                std::chrono::duration_cast<std::chrono::system_clock::duration>(
                    std::chrono::nanoseconds(ns)));
        }
        default:
            throw std::runtime_error("row_codec: unknown value tag");
    }
}

}  // namespace

std::vector<uint8_t> EncodeRow(const Row& row) {
    std::vector<uint8_t> out;
    out.reserve(64 + row.key.size() + row.columns.size() * 24);
    PutString(out, row.key);
    PutU32(out, static_cast<uint32_t>(row.columns.size()));
    for (const auto& [name, value] : row.columns) {
        PutString(out, name);
        EncodeValue(out, value);
    }
    return out;
}

Row DecodeRow(const uint8_t* data, size_t len) {
    Reader r(data, len);
    Row row;
    row.key = r.Str();
    uint32_t count = r.U32();
    for (uint32_t i = 0; i < count; i++) {
        std::string name = r.Str();
        row.columns[name] = DecodeValue(r);
    }
    return row;
}

std::vector<uint8_t> EncodeSchema(const TableSchema& schema) {
    std::vector<uint8_t> out;
    PutString(out, schema.name);
    PutU32(out, static_cast<uint32_t>(schema.primary_key.size()));
    for (const auto& pk : schema.primary_key) PutString(out, pk);
    PutU32(out, static_cast<uint32_t>(schema.columns.size()));
    for (const auto& col : schema.columns) {
        PutString(out, col.name);
        PutU8(out, static_cast<uint8_t>(col.type));
        PutU8(out, col.nullable ? 1 : 0);
        PutU8(out, col.is_primary_key ? 1 : 0);
    }
    return out;
}

TableSchema DecodeSchema(const uint8_t* data, size_t len) {
    Reader r(data, len);
    TableSchema schema;
    schema.name = r.Str();
    uint32_t pk_count = r.U32();
    for (uint32_t i = 0; i < pk_count; i++) schema.primary_key.push_back(r.Str());
    uint32_t col_count = r.U32();
    for (uint32_t i = 0; i < col_count; i++) {
        Column col;
        col.name = r.Str();
        col.type = static_cast<DataType>(r.U8());
        col.nullable = static_cast<bool>(r.U8());
        col.is_primary_key = static_cast<bool>(r.U8());
        schema.columns.push_back(col);
    }
    return schema;
}

}  // namespace origindb
