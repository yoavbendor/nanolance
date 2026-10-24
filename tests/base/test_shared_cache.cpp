// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The bounded shared cache of loaded objects (base/shared_cache.hpp): find and put by key, the value
// handed back is the one put (shared, not copied), and the cache never holds more than 64 entries.
// Readers of an index or a manifest rely on the first two; the bound keeps a long-lived process from
// growing its cache without limit.

#include "base/shared_cache.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

std::shared_ptr<const int> boxed(int v) { return std::make_shared<const int>(v); }

}  // namespace

int main() {
    int checks = 0;

    // Empty cache: nothing found.
    {
        nano_lance::SharedCache<int> cache;
        require(cache.find("missing") == nullptr, "a miss on an empty cache is null");
        ++checks;
    }

    // Put then find: the same object comes back (shared, not copied), and other keys still miss.
    {
        nano_lance::SharedCache<int> cache;
        const auto value = boxed(42);
        cache.put("a", value);
        const auto found = cache.find("a");
        require(found != nullptr, "a put key is found");
        require(found.get() == value.get(), "find returns the object that was put");
        require(*found == 42, "the value is 42");
        require(cache.find("b") == nullptr, "a different key misses");
        require(cache.find("A") == nullptr, "keys are case-sensitive");
        require(cache.find("") == nullptr, "the empty key misses until put");
        checks += 6;
    }

    // Replacing a key: the new value wins, and the empty key is an ordinary key.
    {
        nano_lance::SharedCache<int> cache;
        cache.put("k", boxed(1));
        cache.put("k", boxed(2));
        require(cache.find("k") != nullptr && *cache.find("k") == 2, "a replaced key returns the new value");
        cache.put("", boxed(9));
        require(cache.find("") != nullptr && *cache.find("") == 9, "the empty key can be stored");
        checks += 2;
    }

    // The bound: 64 keys all fit; a 65th put leaves exactly 64 of the 65 keys findable (which one is
    // evicted is unspecified).
    {
        nano_lance::SharedCache<int> cache;
        for (int i = 0; i < 64; ++i) {
            cache.put("k" + std::to_string(i), boxed(i));
        }
        int present = 0;
        for (int i = 0; i < 64; ++i) {
            present += cache.find("k" + std::to_string(i)) != nullptr ? 1 : 0;
        }
        require(present == 64, "all 64 entries fit the bound");
        cache.put("k64", boxed(64));
        require(cache.find("k64") != nullptr, "the 65th entry is stored");
        present = 0;
        for (int i = 0; i <= 64; ++i) {
            present += cache.find("k" + std::to_string(i)) != nullptr ? 1 : 0;
        }
        require(present == 64, "the 65th put keeps the cache at exactly 64 entries");
        checks += 4;
    }

    // Many puts: exactly 64 remain, and the entry just put is always found.
    {
        nano_lance::SharedCache<int> cache;
        for (int i = 0; i < 1000; ++i) {
            cache.put("n" + std::to_string(i), boxed(i));
            require(cache.find("n" + std::to_string(i)) != nullptr, "the entry just put is found");
        }
        int present = 0;
        for (int i = 0; i < 1000; ++i) {
            present += cache.find("n" + std::to_string(i)) != nullptr ? 1 : 0;
        }
        require(present == 64, "after 1000 distinct puts exactly 64 entries remain");
        ++checks;
    }

    // Replacing a key in a full cache keeps every other entry (put used to evict one before it looked at the key).
    {
        nano_lance::SharedCache<int> cache;
        for (int i = 0; i < 64; ++i) {
            cache.put("k" + std::to_string(i), boxed(i));
        }
        cache.put("k0", boxed(100));
        int present = 0;
        for (int i = 0; i < 64; ++i) {
            present += cache.find("k" + std::to_string(i)) != nullptr ? 1 : 0;
        }
        require(present == 64, "replacing a key in a full cache keeps all 64 keys (" + std::to_string(present) + " remain)");
        require(*cache.find("k0") == 100, "the replaced key holds its new value");
        checks += 2;
    }

    std::cout << "shared_cache: " << checks << " checks passed\n";
    return 0;
}
