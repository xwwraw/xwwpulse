//  Fixed-size least-recently-used map. Verdicts are keyed by ProcessId
//  (PID + creation time), so a reused PID never inherits a dead process's
//  verdict.
#pragma once

#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace xww {

template <class Key, class Value, class Hash = std::hash<Key>>
class LruCache {
public:
    explicit LruCache(size_t capacity) : capacity_(capacity) {}

    std::optional<Value> Get(const Key& key) {
        std::lock_guard lock(gate_);
        auto it = index_.find(key);
        if (it == index_.end()) return std::nullopt;
        order_.splice(order_.begin(), order_, it->second);     // most recent first
        return it->second->second;
    }

    void Put(const Key& key, Value value) {
        std::lock_guard lock(gate_);
        if (auto it = index_.find(key); it != index_.end()) {
            it->second->second = std::move(value);
            order_.splice(order_.begin(), order_, it->second);
            return;
        }
        order_.emplace_front(key, std::move(value));
        index_[key] = order_.begin();
        if (order_.size() > capacity_) {
            index_.erase(order_.back().first);
            order_.pop_back();
        }
    }

    void Erase(const Key& key) {
        std::lock_guard lock(gate_);
        if (auto it = index_.find(key); it != index_.end()) { order_.erase(it->second); index_.erase(it); }
    }

    template <class Pred> void EraseIf(Pred pred) {
        std::lock_guard lock(gate_);
        for (auto it = order_.begin(); it != order_.end();) {
            if (pred(it->first)) { index_.erase(it->first); it = order_.erase(it); }
            else ++it;
        }
    }

    void Clear() { std::lock_guard lock(gate_); order_.clear(); index_.clear(); }

private:
    size_t capacity_;
    std::mutex gate_;
    std::list<std::pair<Key, Value>> order_;
    std::unordered_map<Key, typename std::list<std::pair<Key, Value>>::iterator, Hash> index_;
};

}  // namespace xww
