#pragma once

#include "hash.hpp"
#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace hashbench {

struct Person {
    std::string name;
    std::uint64_t age;
};

inline std::string hex_key(std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (std::size_t i = 16; i-- > 0;) {
        result[i] = digits[value & 15];
        value >>= 4;
    }
    return result;
}

// Use our own shuffle so the same seed gives the same order across libraries
// Reject the small biased part of the random range before choosing an index
template<class T>
void deterministic_shuffle(std::vector<T>& items, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    for (std::size_t i = items.size(); i > 1; --i) {
        const auto bound = static_cast<std::uint64_t>(i);
        const auto threshold = (UINT64_C(0) - bound) % bound;
        std::uint64_t draw;
        do { draw = random(); } while (draw < threshold);
        std::swap(items[i - 1], items[draw % bound]);
    }
}

template<class Key, class Value>
struct Dataset {
    Dataset() = default;
    Dataset(const Dataset&) = delete;
    Dataset& operator=(const Dataset&) = delete;
    Dataset(Dataset&&) = default;
    Dataset& operator=(Dataset&&) = default;

    std::vector<Person> people; // sole payload owner; completed before pointers escape
    std::vector<std::pair<Key, Value>> entries;
    std::vector<Key> hit_queries;
    std::vector<Key> miss_queries;
    std::uint64_t fingerprint = 0;
};

inline std::uint64_t key_id(std::uint64_t i, const std::string& distribution,
                            std::uint64_t seed) {
    // Mixing doesn't repeat values, so the random dataset still has unique keys
    // Generate misses from a separate range to keep them out of the table
    return distribution == "random" ? mix64(i + seed) : i;
}

template<class Key>
Key make_key(std::uint64_t id) {
    if constexpr (std::is_same_v<Key, std::string>) return hex_key(id);
    else return id;
}

template<class Key, class Value>
Dataset<Key, Value> make_dataset(std::size_t count, const std::string& distribution,
                                 std::uint64_t seed) {
    Dataset<Key, Value> data;
    data.entries.reserve(count);
    data.people.reserve(std::is_pointer_v<Value> ? count : 0);
    data.hit_queries.reserve(count);
    data.miss_queries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto id = key_id(i, distribution, seed);
        auto key = make_key<Key>(id);
        if constexpr (std::is_pointer_v<Value>) {
            data.people.push_back({key, id % 99 + 1});
            data.entries.emplace_back(key, &data.people.back());
        } else {
            data.entries.emplace_back(key, mix64(id ^ UINT64_C(0x9e3779b97f4a7c15)));
        }
        data.hit_queries.push_back(key);
        data.miss_queries.push_back(make_key<Key>(key_id(count + i, distribution, seed)));
    }
    if (distribution == "shuffled") deterministic_shuffle(data.entries, seed);
    // Hash the data and insertion order so runs can check they used the same input
    SharedHash<Key> hash;
    for (const auto& entry : data.entries) {
        std::uint64_t value;
        if constexpr (std::is_pointer_v<Value>) value = entry.second->age;
        else value = entry.second;
        data.fingerprint = mix64(data.fingerprint ^ hash(entry.first) ^ value);
    }
    deterministic_shuffle(data.hit_queries, seed ^ UINT64_C(0xa0761d6478bd642f));
    deterministic_shuffle(data.miss_queries, seed ^ UINT64_C(0xe7037ed1a0b428db));
    return data;
}

template<class Value>
std::uint64_t value_checksum(const Value& value) {
    if constexpr (std::is_pointer_v<Value>) return value->age;
    else return value;
}

} // namespace hashbench
