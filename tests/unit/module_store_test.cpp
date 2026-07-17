#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "wasm/module_store.h"

namespace origindb {
namespace {

class ModuleStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("module_store_test_" +
                std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::filesystem::path dir_;
    std::vector<uint8_t> bytes_{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
};

TEST_F(ModuleStoreTest, SaveLoadRoundTrip) {
    ModuleStore store(dir_);
    ASSERT_TRUE(store.Initialize());

    std::string error;
    ASSERT_TRUE(store.Save("mod", "1.2.3", bytes_, error)) << error;

    auto info = store.GetInfo("mod");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->version, "1.2.3");
    EXPECT_EQ(info->size_bytes, bytes_.size());
    EXPECT_EQ(info->sha256, ModuleStore::Sha256Hex(bytes_.data(), bytes_.size()));

    auto loaded = store.LoadBytecode("mod", error);
    ASSERT_TRUE(loaded.has_value()) << error;
    EXPECT_EQ(*loaded, bytes_);
}

TEST_F(ModuleStoreTest, SurvivesRestart) {
    std::string error;
    {
        ModuleStore store(dir_);
        ASSERT_TRUE(store.Initialize());
        ASSERT_TRUE(store.Save("mod", "1.0", bytes_, error)) << error;
    }
    ModuleStore reopened(dir_);
    ASSERT_TRUE(reopened.Initialize());
    EXPECT_EQ(reopened.List().size(), 1u);
    EXPECT_TRUE(reopened.GetInfo("mod").has_value());
}

TEST_F(ModuleStoreTest, RedeployOverwrites) {
    ModuleStore store(dir_);
    ASSERT_TRUE(store.Initialize());
    std::string error;
    ASSERT_TRUE(store.Save("mod", "1.0", bytes_, error));

    std::vector<uint8_t> v2 = bytes_;
    v2.push_back(0xFF);
    ASSERT_TRUE(store.Save("mod", "2.0", v2, error));

    EXPECT_EQ(store.List().size(), 1u);
    auto info = store.GetInfo("mod");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->version, "2.0");
    EXPECT_EQ(info->size_bytes, v2.size());
}

TEST_F(ModuleStoreTest, RemoveDeletesFiles) {
    ModuleStore store(dir_);
    ASSERT_TRUE(store.Initialize());
    std::string error;
    ASSERT_TRUE(store.Save("mod", "1.0", bytes_, error));
    EXPECT_TRUE(store.Remove("mod"));
    EXPECT_FALSE(store.GetInfo("mod").has_value());
    EXPECT_FALSE(std::filesystem::exists(dir_ / "mod"));
    EXPECT_FALSE(store.Remove("mod"));
}

TEST_F(ModuleStoreTest, SkipsCorruptEntriesOnInitialize) {
    std::string error;
    {
        ModuleStore store(dir_);
        ASSERT_TRUE(store.Initialize());
        ASSERT_TRUE(store.Save("good", "1.0", bytes_, error));
        ASSERT_TRUE(store.Save("tampered", "1.0", bytes_, error));
    }
    {
        std::ofstream out(dir_ / "tampered" / "module.wasm",
                          std::ios::binary | std::ios::trunc);
        out << "not the original bytes";
    }
    std::filesystem::create_directories(dir_ / "no_manifest");

    ModuleStore reopened(dir_);
    ASSERT_TRUE(reopened.Initialize());
    EXPECT_EQ(reopened.List().size(), 1u);
    EXPECT_TRUE(reopened.GetInfo("good").has_value());
    EXPECT_FALSE(reopened.GetInfo("tampered").has_value());
}

TEST_F(ModuleStoreTest, RejectsBadNames) {
    ModuleStore store(dir_);
    ASSERT_TRUE(store.Initialize());
    std::string error;
    EXPECT_FALSE(store.Save("", "1.0", bytes_, error));
    EXPECT_FALSE(store.Save("../escape", "1.0", bytes_, error));
    EXPECT_FALSE(store.Save("a/b", "1.0", bytes_, error));
}

}  // namespace
}  // namespace origindb
