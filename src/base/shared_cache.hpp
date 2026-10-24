// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// A bounded cache of shared, immutable loaded objects, by key (internal).

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace nano_lance {

/// A small bounded cache of shared, immutable loaded objects by key.
template <typename T>
class SharedCache {
public:
    std::shared_ptr<const T> find(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(key);
        return it == entries_.end() ? nullptr : it->second;
    }
    void put(const std::string& key, std::shared_ptr<const T> value) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Replacing a key leaves the size as it is; only a new key over the bound evicts another.
        if (entries_.size() >= 64U && entries_.find(key) == entries_.end()) {
            entries_.erase(entries_.begin());
        }
        entries_[key] = std::move(value);
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const T>> entries_;
};

}  // namespace nano_lance
