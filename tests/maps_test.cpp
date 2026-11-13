#include "hashbench/dataset.hpp"
#include "hashbench/library_maps.hpp"
#include "hashbench/maps.hpp"
#include <gtest/gtest.h>
#include <random>
#include <set>
#include <unordered_map>

namespace hb = hashbench;

// Send every key to the same bucket so collisions are easy to check
struct ConstantHash {
    template<class Key>
    std::size_t operator()(const Key&) const { return 7; }
};
using Linear = hb::LinearMap<std::uint64_t, std::uint64_t, ConstantHash>;
using Chained = hb::ChainedMap<std::uint64_t, std::uint64_t, ConstantHash>;

// Both maps should give the same results for insertions and updates
// Pass in a map so these checks don't have to be repeated for each one
template<class Map>
void check_collisions(Map& map) {
    for (std::uint64_t i = 0; i < 8; ++i) EXPECT_TRUE(map.insert_or_assign(i, i + 10));
    EXPECT_EQ(map.size(), 8);
    EXPECT_FALSE(map.insert_or_assign(0, 999));
    ASSERT_NE(map.find(0), nullptr);
    EXPECT_EQ(*map.find(0), 999);
    EXPECT_EQ(map.find(100), nullptr);
    for (std::uint64_t i = 1; i < 8; ++i) {
        ASSERT_NE(map.find(i), nullptr);
        EXPECT_EQ(*map.find(i), i + 10);
    }
}

template<class Map>
void check_growth(Map& map) {
    std::unordered_map<std::uint64_t, std::uint64_t> reference;
    std::mt19937_64 random(123);
    for (int i = 0; i < 3000; ++i) {
        auto key = random() % 300;
        auto value = random();
        EXPECT_EQ(map.insert_or_assign(key, value), reference.insert_or_assign(key, value).second);
        if (i % 100 != 0) continue;
        ASSERT_EQ(map.size(), reference.size());
        for (std::uint64_t k = 0; k < 350; ++k) {
            auto expected = reference.find(k);
            auto actual = map.find(k);
            ASSERT_EQ(actual != nullptr, expected != reference.end());
            if (actual) {
                EXPECT_EQ(*actual, expected->second);
            }
        }
    }
    EXPECT_LE(map.load_factor(), 0.7);
}

TEST(LinearMap, HandlesCollisions) {
    Linear map(8, 0.8, false);
    check_collisions(map);
}
TEST(ChainedMap, HandlesCollisions) {
    Chained map(8, 0.8, false);
    check_collisions(map);
}
TEST(LinearMap, GrowsAndMatchesStdMap) {
    Linear map(1, 0.7, true);
    check_growth(map);
}
TEST(ChainedMap, GrowsAndMatchesStdMap) {
    Chained map(1, 0.7, true);
    check_growth(map);
}
TEST(Maps, RespectsSmallLoadLimits) {
    Linear linear(1, 0.01, true);
    Chained chained(1, 0.01, true);
    linear.insert_or_assign(1, 2);
    chained.insert_or_assign(1, 2);
    EXPECT_LE(linear.load_factor(), 0.01);
    EXPECT_LE(chained.load_factor(), 0.01);
}
TEST(Maps, LeavesPointerValuesAlive) {
    hb::Person person{"same", 25};
    {
        hb::LinearMap<std::string, hb::Person*, ConstantHash> linear(4, 0.8, true);
        hb::ChainedMap<std::string, hb::Person*, ConstantHash> chained(4, 0.8, true);
        linear.insert_or_assign("same", &person);
        chained.insert_or_assign("same", &person);
        ASSERT_NE(linear.find("same"), nullptr);
        ASSERT_NE(chained.find("same"), nullptr);
        EXPECT_EQ(*linear.find("same"), &person);
        EXPECT_EQ(*chained.find("same"), &person);
    }
    EXPECT_EQ(person.age, 25);
}
TEST(LinearMap, UpdatesWhenFull) {
    Linear map(2, 0.8, false);
    map.insert_or_assign(1, 1);
    map.insert_or_assign(2, 2);
    EXPECT_THROW(map.insert_or_assign(3, 3), std::overflow_error);
    EXPECT_FALSE(map.insert_or_assign(1, 4));
    EXPECT_EQ(map.size(), 2);
    ASSERT_NE(map.find(1), nullptr);
    EXPECT_EQ(*map.find(1), 4);
    EXPECT_EQ(map.find(3), nullptr);
}
TEST(LinearMap, HandlesCollidingStrings) {
    hb::LinearMap<std::string, int, hb::SharedHash<std::string>> map(8, 0.8, false, {true});
    for (auto key : {"abcd", "abdc", "acbd", "acdb"}) map.insert_or_assign(key, 1);
    EXPECT_EQ(map.size(), 4);
    EXPECT_NE(map.find("acdb"), nullptr);
    EXPECT_EQ(map.diagnostics().max_successful_probes, 4);
    EXPECT_DOUBLE_EQ(*map.diagnostics().mean_successful_probes, 2.5);
}
TEST(ChainedMap, HandlesLongChains) {
    Chained map(1, 0.8, false);
    for (int i = 0; i < 6000; ++i) map.insert_or_assign(i, i);
    EXPECT_EQ(map.diagnostics().max_chain_length, 6000);
    EXPECT_DOUBLE_EQ(*map.diagnostics().mean_successful_probes, 3000.5);
}

template<bool Sparse>
void check_library() {
    hb::LibraryMap<std::string, std::uint64_t, ConstantHash, Sparse> map(10, 0.8, true);
    EXPECT_TRUE(map.insert_or_assign("", 1));
    EXPECT_FALSE(map.insert_or_assign("", 2));
    ASSERT_NE(map.find(""), nullptr);
    EXPECT_EQ(*map.find(""), 2);
    EXPECT_EQ(map.size(), 1);
    EXPECT_EQ(map.find("absent"), nullptr);
    for (std::uint64_t i = 0; i < 100; ++i) map.insert_or_assign(std::to_string(i), i);
    for (std::uint64_t i = 0; i < 100; ++i) {
        ASSERT_NE(map.find(std::to_string(i)), nullptr);
        EXPECT_EQ(*map.find(std::to_string(i)), i);
    }
}
TEST(LibraryMaps, StdMap) { check_library<false>(); }
TEST(LibraryMaps, Sparsepp) { check_library<true>(); }
TEST(Dataset, ShufflePreservesEntries) {
    auto sequential = hb::make_dataset<std::uint64_t, std::uint64_t>(100, "sequential", 42);
    auto shuffled = hb::make_dataset<std::uint64_t, std::uint64_t>(100, "shuffled", 42);
    EXPECT_NE(sequential.fingerprint, shuffled.fingerprint);
    std::sort(sequential.entries.begin(), sequential.entries.end());
    std::sort(shuffled.entries.begin(), shuffled.entries.end());
    EXPECT_EQ(sequential.entries, shuffled.entries);
}
TEST(Dataset, RandomKeysAreUniqueAndMissesAreAbsent) {
    auto data = hb::make_dataset<std::uint64_t, std::uint64_t>(500, "random", UINT64_MAX);
    std::set<std::uint64_t> keys(data.hit_queries.begin(), data.hit_queries.end());
    EXPECT_EQ(keys.size(), 500);
    for (auto key : data.miss_queries) EXPECT_EQ(keys.count(key), 0);
    EXPECT_EQ(data.fingerprint, (hb::make_dataset<std::uint64_t, std::uint64_t>(500, "random", UINT64_MAX).fingerprint));
}
TEST(Dataset, MovingKeepsPointersValid) {
    auto original = hb::make_dataset<std::string, hb::Person*>(100, "shuffled", 42);
    auto moved = std::move(original);
    for (const auto& entry : moved.entries) EXPECT_EQ(entry.first, entry.second->name);
}
TEST(Configuration, RejectsInvalidLoadLimits) {
    EXPECT_THROW(hb::capacity_for(100, 0), std::invalid_argument);
    EXPECT_THROW(hb::capacity_for(100, 1), std::invalid_argument);
    EXPECT_THROW(hb::capacity_for(100, std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
}
