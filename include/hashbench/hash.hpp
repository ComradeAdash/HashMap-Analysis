#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace hashbench {

// Mix every input bit into the hash so nearby integers don't all land nearby
// The arithmetic wraps on purpose, and each input still has a unique output
inline std::uint64_t mix64(std::uint64_t x) noexcept {
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

template<class Key>
struct SharedHash {
    bool weak = false;

    std::size_t operator()(const Key& key) const noexcept {
        if constexpr (std::is_same_v<Key, std::uint64_t>) {
            return static_cast<std::size_t>(weak ? key : mix64(key));
        } else {
            static_assert(std::is_same_v<Key, std::string>);
            std::uint64_t hash = weak ? 0 : UINT64_C(14695981039346656037);
            for (unsigned char ch : key) {
                if (weak) {
                    hash += ch;
                } else {
                    hash = (hash ^ ch) * UINT64_C(1099511628211);
                }
            }
            return static_cast<std::size_t>(weak ? hash : mix64(hash));
        }
    }
};

} // namespace hashbench
