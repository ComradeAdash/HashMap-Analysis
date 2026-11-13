#pragma once

#include "maps.hpp"
#include <sparsepp/spp.h>
#include <unordered_map>

namespace hashbench {

template<class Key, class Value, class Hash, bool Sparse>
class LibraryMap {
    using Map = std::conditional_t<Sparse,
        spp::sparse_hash_map<Key, Value, Hash>, std::unordered_map<Key, Value, Hash>>;
    Map map_;

public:
    LibraryMap(std::size_t count, double load, bool reserve, Hash hash = {})
        : map_(0, hash) {
        validate_load(load);
        // Set the load limit before reserving since it affects the space needed
        // The libraries choose their own bucket counts, so record the actual load
        map_.max_load_factor(static_cast<float>(load));
        if (reserve) map_.reserve(count);
    }

    bool insert_or_assign(const Key& key, const Value& value) {
        if constexpr (Sparse) {
            auto result = map_.insert({key, value});
            if (!result.second) result.first->second = value;
            return result.second;
        } else {
            return map_.insert_or_assign(key, value).second;
        }
    }

    const Value* find(const Key& key) const {
        auto found = map_.find(key);
        return found == map_.end() ? nullptr : &found->second;
    }

    std::size_t size() const noexcept { return map_.size(); }
    std::size_t capacity() const noexcept { return map_.bucket_count(); }
    double load_factor() const { return map_.load_factor(); }
    Diagnostics diagnostics() const { return {}; }
};

} // namespace hashbench
