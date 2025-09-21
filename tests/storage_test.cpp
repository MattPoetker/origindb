#include <gtest/gtest.h>
#include "storage/storage_engine.h"

using namespace instantdb;

class StorageEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        StorageConfig config;
        config.data_dir = "/tmp/instantdb_test";
        engine = std::make_unique<StorageEngine>(config);
        ASSERT_TRUE(engine->Initialize());
    }

    void TearDown() override {
        engine->Shutdown();
    }

    std::unique_ptr<StorageEngine> engine;
};

TEST_F(StorageEngineTest, CreateTable) {
    TableSchema schema;
    schema.name = "test_table";
    schema.columns = {
        {"id", DataType::INT64, false, true},
        {"name", DataType::STRING, false},
        {"value", DataType::DOUBLE, true}
    };
    schema.primary_key = {"id"};

    EXPECT_TRUE(engine->CreateTable(schema));

    auto table = engine->GetTable("test_table");
    EXPECT_NE(table, nullptr);
    EXPECT_EQ(table->GetSchema().name, "test_table");
}

TEST_F(StorageEngineTest, BasicOperations) {
    // Create test table
    TableSchema schema;
    schema.name = "users";
    schema.columns = {
        {"id", DataType::INT64, false, true},
        {"name", DataType::STRING, false}
    };
    schema.primary_key = {"id"};

    ASSERT_TRUE(engine->CreateTable(schema));

    // Insert row
    Row row;
    row.key = "1";
    row.columns["id"] = int64_t(1);
    row.columns["name"] = std::string("Alice");

    EXPECT_TRUE(engine->Insert("users", row));

    // Get row
    auto result = engine->Get("users", "1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<int64_t>(result->columns.at("id")), 1);
    EXPECT_EQ(std::get<std::string>(result->columns.at("name")), "Alice");
}

TEST_F(StorageEngineTest, Transactions) {
    // Create test table
    TableSchema schema;
    schema.name = "accounts";
    schema.columns = {
        {"id", DataType::STRING, false, true},
        {"balance", DataType::DOUBLE, false}
    };
    schema.primary_key = {"id"};

    ASSERT_TRUE(engine->CreateTable(schema));

    // Begin transaction
    auto txn = engine->BeginTransaction(IsolationLevel::SNAPSHOT);
    ASSERT_NE(txn, nullptr);

    // Insert within transaction
    Row row;
    row.key = "acc1";
    row.columns["id"] = std::string("acc1");
    row.columns["balance"] = 100.0;

    EXPECT_TRUE(txn->Insert("accounts", row));

    // Commit transaction
    EXPECT_TRUE(engine->Commit(txn));

    // Verify data persisted
    auto result = engine->Get("accounts", "acc1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<double>(result->columns.at("balance")), 100.0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}