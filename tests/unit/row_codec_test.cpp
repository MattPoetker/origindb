// Round-trip tests for the binary row/schema codec used by the WAL + snapshot.
#include <gtest/gtest.h>

#include <chrono>

#include "storage/row_codec.h"

namespace origindb {
namespace {

TEST(RowCodec, RoundTripsEveryValueType) {
    Row row;
    row.key = "user:42";
    row.columns["null_col"] = std::monostate{};
    row.columns["i32"] = static_cast<int32_t>(-12345);
    row.columns["i64"] = static_cast<int64_t>(9000000000LL);
    row.columns["f32"] = 3.5f;
    row.columns["f64"] = 2.718281828;
    row.columns["str"] = std::string("hello \"world\" \n\t binary-ish");
    row.columns["bytes"] = std::vector<uint8_t>{0x00, 0xff, 0x10, 0x00, 0x7f};
    row.columns["flag"] = true;
    // µs-aligned so it round-trips exactly regardless of system_clock's tick
    // (macOS uses microseconds; the codec stores nanoseconds).
    auto tp = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::microseconds(1'700'000'000'123'456LL)));
    row.columns["ts"] = tp;

    auto encoded = EncodeRow(row);
    Row decoded = DecodeRow(encoded);

    EXPECT_EQ(decoded.key, row.key);
    ASSERT_EQ(decoded.columns.size(), row.columns.size());

    // Exact type preservation (JSON path collapsed int32->int64, float->double).
    EXPECT_TRUE(std::holds_alternative<std::monostate>(decoded.columns["null_col"]));
    EXPECT_EQ(std::get<int32_t>(decoded.columns["i32"]), -12345);
    EXPECT_EQ(std::get<int64_t>(decoded.columns["i64"]), 9000000000LL);
    EXPECT_FLOAT_EQ(std::get<float>(decoded.columns["f32"]), 3.5f);
    EXPECT_DOUBLE_EQ(std::get<double>(decoded.columns["f64"]), 2.718281828);
    EXPECT_EQ(std::get<std::string>(decoded.columns["str"]),
              std::get<std::string>(row.columns["str"]));
    EXPECT_EQ(std::get<std::vector<uint8_t>>(decoded.columns["bytes"]),
              (std::vector<uint8_t>{0x00, 0xff, 0x10, 0x00, 0x7f}));
    EXPECT_EQ(std::get<bool>(decoded.columns["flag"]), true);
    EXPECT_EQ(std::get<std::chrono::system_clock::time_point>(decoded.columns["ts"]),
              tp);
}

TEST(RowCodec, EmptyRow) {
    Row row;
    row.key = "";
    auto decoded = DecodeRow(EncodeRow(row));
    EXPECT_EQ(decoded.key, "");
    EXPECT_TRUE(decoded.columns.empty());
}

TEST(RowCodec, TruncatedInputThrows) {
    Row row;
    row.key = "k";
    row.columns["a"] = std::string("value");
    auto encoded = EncodeRow(row);
    encoded.resize(encoded.size() / 2);  // chop it in half
    EXPECT_THROW(DecodeRow(encoded), std::runtime_error);
}

TEST(RowCodec, SchemaRoundTrip) {
    TableSchema schema;
    schema.name = "notes";
    schema.primary_key = {"id"};
    schema.columns = {
        {.name = "id", .type = DataType::INT64, .nullable = false,
         .is_primary_key = true},
        {.name = "text", .type = DataType::STRING, .nullable = true},
        {.name = "x", .type = DataType::DOUBLE},
    };

    auto decoded = DecodeSchema(EncodeSchema(schema));
    EXPECT_EQ(decoded.name, "notes");
    ASSERT_EQ(decoded.primary_key.size(), 1u);
    EXPECT_EQ(decoded.primary_key[0], "id");
    ASSERT_EQ(decoded.columns.size(), 3u);
    EXPECT_EQ(decoded.columns[0].name, "id");
    EXPECT_EQ(decoded.columns[0].type, DataType::INT64);
    EXPECT_TRUE(decoded.columns[0].is_primary_key);
    EXPECT_EQ(decoded.columns[1].name, "text");
    EXPECT_TRUE(decoded.columns[1].nullable);
    EXPECT_EQ(decoded.columns[2].type, DataType::DOUBLE);
}

}  // namespace
}  // namespace origindb
