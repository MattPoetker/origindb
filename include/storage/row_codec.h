#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "storage/table.h"

namespace origindb {

// Compact little-endian binary codec for rows and schemas — OriginDB's answer
// to the JSON-hex WAL encoding it replaces. Used for the WAL and snapshot
// (internal, on-disk) hot paths. The changefeed wire format stays JSON, so
// external consumers (browsers, the SQL predicate evaluator) are unaffected.
//
// Format is self-describing per value (a 1-byte type tag), so it round-trips
// every Value alternative exactly — including the int32 vs int64 and float vs
// double distinctions the old JSON path collapsed.
//
// Row layout:
//   [u32 key_len][key bytes]
//   [u32 column_count]
//   repeated column_count times:
//     [u32 name_len][name bytes][u8 type_tag][value bytes...]
//
// type_tag values (match the Value variant order):
//   0 null, 1 int32, 2 int64, 3 float, 4 double, 5 string, 6 bytes, 7 bool,
//   8 timestamp (int64 ns since epoch)

std::vector<uint8_t> EncodeRow(const Row& row);
Row DecodeRow(const uint8_t* data, size_t len);

inline Row DecodeRow(const std::vector<uint8_t>& data) {
    return DecodeRow(data.data(), data.size());
}

// Schema codec (used by snapshots). Layout:
//   [u32 name_len][name]
//   [u32 pk_count] repeated: [u32 len][pk column name]
//   [u32 column_count] repeated:
//     [u32 name_len][name][u8 type][u8 nullable][u8 is_primary_key]
std::vector<uint8_t> EncodeSchema(const TableSchema& schema);
TableSchema DecodeSchema(const uint8_t* data, size_t len);

inline TableSchema DecodeSchema(const std::vector<uint8_t>& data) {
    return DecodeSchema(data.data(), data.size());
}

}  // namespace origindb
