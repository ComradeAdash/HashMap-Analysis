#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace hashbench {

struct Diagnostics {
    std::optional<std::size_t> max_chain_length;
    std::optional<std::size_t> max_successful_probes;
    std::optional<double> mean_successful_probes;
};

inline void validate_load(double load) {
    if (!std::isfinite(load) || load <= 0.0 || load >= 1.0) {
        throw std::invalid_argument("max load must be finite and strictly between 0 and 1");
    }
}

inline std::size_t capacity_for(std::size_t count, double load) {
    validate_load(load);
    const long double required = std::ceil(static_cast<long double>(count) / load);
    if (required >= static_cast<long double>(std::numeric_limits<std::size_t>::max())) {
        throw std::length_error("requested capacity is too large");
    }
    return std::max<std::size_t>(8, static_cast<std::size_t>(required));
}

inline std::size_t doubled_capacity(std::size_t current) {
    if (current > std::numeric_limits<std::size_t>::max() / 2) {
        throw std::length_error("table capacity overflow");
    }
    return current * 2;
}

// Keep keys and values together in one array so probing visits nearby slots
// Empty slots use optional since this benchmark doesn't need erase or tombstones
template<class Key, class Value, class Hash = std::hash<Key>>
class LinearMap {
    using Entry = std::pair<Key, Value>;
    std::vector<std::optional<Entry>> slots_;
    std::size_t size_ = 0;
    Hash hash_;
    double max_load_;
    bool grow_;

    void rehash(std::size_t capacity) {
        // Build the new table first so a failed copy leaves the old one usable
        std::vector<std::optional<Entry>> replacement(capacity);
        for (const auto& slot : slots_) {
            if (!slot) continue;
            auto index = hash_(slot->first) % capacity;
            while (replacement[index]) index = (index + 1) % capacity;
            replacement[index].emplace(*slot);
        }
        slots_.swap(replacement);
    }

public:
    LinearMap(std::size_t capacity, double max_load, bool grow, Hash hash = {})
        : slots_(std::max<std::size_t>(1, capacity)), hash_(hash),
          max_load_(max_load), grow_(grow) { validate_load(max_load); }

    bool insert_or_assign(const Key& key, const Value& value) {
        for (;;) {
            auto index = hash_(key) % slots_.size();
            // Stop after one trip around the table, even if every slot is taken
            for (std::size_t visited = 0; visited < slots_.size(); ++visited) {
                auto& slot = slots_[index];
                if (!slot) {
                    if (grow_ && static_cast<long double>(size_ + 1) >
                                     max_load_ * static_cast<long double>(slots_.size())) {
                        rehash(std::max(doubled_capacity(slots_.size()),
                                        capacity_for(size_ + 1, max_load_)));
                        break;
                    }
                    slot.emplace(key, value);
                    ++size_;
                    return true;
                }
                if (slot->first == key) {
                    slot->second = value;
                    return false;
                }
                index = (index + 1) % slots_.size();
            }
            if (!grow_) throw std::overflow_error("fixed linear table is full");
            // If the load check already grew the table, just retry the insertion
            // Otherwise a full table needs more room before we try again
            if (size_ == slots_.size()) rehash(doubled_capacity(slots_.size()));
        }
    }

    const Value* find(const Key& key) const {
        auto index = hash_(key) % slots_.size();
        for (std::size_t visited = 0; visited < slots_.size(); ++visited) {
            const auto& slot = slots_[index];
            if (!slot) return nullptr;
            if (slot->first == key) return &slot->second;
            index = (index + 1) % slots_.size();
        }
        return nullptr;
    }

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return slots_.size(); }
    double load_factor() const { return static_cast<double>(size_) / capacity(); }

    Diagnostics diagnostics() const {
        std::size_t maximum = 0;
        long double total = 0;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (!slots_[i]) continue;
            const auto home = hash_(slots_[i]->first) % slots_.size();
            const auto distance = i >= home ? i - home : slots_.size() - home + i;
            maximum = std::max(maximum, distance + 1);
            total += distance + 1;
        }
        return {std::nullopt, maximum, size_ ? static_cast<double>(total / size_) : 0.0};
    }
};

// Each bucket owns its nodes, and each node stores a key and value
// A pointer value belongs to the caller, so destroying a map won't delete a Person
template<class Key, class Value, class Hash = std::hash<Key>>
class ChainedMap {
    struct Node {
        Key key;
        Value value;
        std::unique_ptr<Node> next;
        Node(const Key& k, const Value& v) : key(k), value(v) {}
    };
    std::vector<std::unique_ptr<Node>> buckets_;
    std::size_t size_ = 0;
    Hash hash_;
    double max_load_;
    bool grow_;

    void rehash(std::size_t capacity) {
        // Work out where nodes will go before changing any of the old links
        // Then move the existing nodes instead of allocating them all again
        std::vector<std::unique_ptr<Node>> replacement(capacity);
        std::vector<std::size_t> destinations;
        destinations.reserve(size_);
        for (const auto& bucket : buckets_) {
            for (auto* node = bucket.get(); node; node = node->next.get()) {
                destinations.push_back(hash_(node->key) % capacity);
            }
        }
        std::size_t i = 0;
        for (auto& bucket : buckets_) {
            while (bucket) {
                auto node = std::move(bucket);
                bucket = std::move(node->next);
                const auto destination = destinations[i++];
                node->next = std::move(replacement[destination]);
                replacement[destination] = std::move(node);
            }
        }
        buckets_.swap(replacement);
    }

public:
    ChainedMap(std::size_t capacity, double max_load, bool grow, Hash hash = {})
        : buckets_(std::max<std::size_t>(1, capacity)), hash_(hash),
          max_load_(max_load), grow_(grow) { validate_load(max_load); }

    ChainedMap(const ChainedMap&) = delete;
    ChainedMap& operator=(const ChainedMap&) = delete;
    ChainedMap(ChainedMap&&) = delete;
    ChainedMap& operator=(ChainedMap&&) = delete;

    ~ChainedMap() {
        // Delete one node at a time so a long chain doesn't exhaust the stack
        for (auto& bucket : buckets_) {
            while (bucket) {
                auto node = std::move(bucket);
                bucket = std::move(node->next);
            }
        }
    }

    bool insert_or_assign(const Key& key, const Value& value) {
        auto index = hash_(key) % buckets_.size();
        for (auto* node = buckets_[index].get(); node; node = node->next.get()) {
            if (node->key == key) {
                node->value = value;
                return false;
            }
        }
        auto node = std::make_unique<Node>(key, value);
        if (grow_ && static_cast<long double>(size_ + 1) >
                         max_load_ * static_cast<long double>(buckets_.size())) {
            rehash(std::max(doubled_capacity(buckets_.size()),
                            capacity_for(size_ + 1, max_load_)));
            index = hash_(key) % buckets_.size();
        }
        node->next = std::move(buckets_[index]);
        buckets_[index] = std::move(node);
        ++size_;
        return true;
    }

    const Value* find(const Key& key) const {
        for (auto* node = buckets_[hash_(key) % buckets_.size()].get();
             node; node = node->next.get()) {
            if (node->key == key) return &node->value;
        }
        return nullptr;
    }

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return buckets_.size(); }
    double load_factor() const { return static_cast<double>(size_) / capacity(); }

    Diagnostics diagnostics() const {
        std::size_t maximum = 0;
        long double total = 0;
        for (const auto& bucket : buckets_) {
            std::size_t length = 0;
            for (auto* node = bucket.get(); node; node = node->next.get()) ++length;
            maximum = std::max(maximum, length);
            total += static_cast<long double>(length) * (length + 1) / 2;
        }
        return {maximum, maximum, size_ ? static_cast<double>(total / size_) : 0.0};
    }
};

} // namespace hashbench
