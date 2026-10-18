#pragma once
// Generic cost-bounded LRU (not thread-safe; wrap with a mutex where shared).
#include <cstddef>
#include <functional>
#include <list>
#include <unordered_map>
#include <utility>

namespace cut {

template <class K, class V, class Hash = std::hash<K>>
class LruCache {
public:
    using EvictFn = std::function<void(const K&, V&)>;
    explicit LruCache(size_t budget = 0) : budget_(budget) {}

    void setBudget(size_t b) {
        budget_ = b;
        trim();
    }
    size_t budget() const { return budget_; }
    size_t cost() const { return cost_; }
    size_t size() const { return map_.size(); }
    void onEvict(EvictFn f) { evict_ = std::move(f); }

    // Inserts or replaces; most recently used. Evicts LRU entries beyond the budget
    // (never the entry just inserted).
    void put(const K& k, V v, size_t cost) {
        auto it = map_.find(k);
        if (it != map_.end()) {
            cost_ -= it->second->cost;
            list_.erase(it->second);
            map_.erase(it);
        }
        list_.push_front({k, std::move(v), cost});
        map_[k] = list_.begin();
        cost_ += cost;
        trim();
    }

    V* get(const K& k) {
        auto it = map_.find(k);
        if (it == map_.end()) return nullptr;
        list_.splice(list_.begin(), list_, it->second);
        return &it->second->value;
    }
    V* peek(const K& k) {
        auto it = map_.find(k);
        return it == map_.end() ? nullptr : &it->second->value;
    }
    bool contains(const K& k) const { return map_.count(k) > 0; }

    bool erase(const K& k) {
        auto it = map_.find(k);
        if (it == map_.end()) return false;
        cost_ -= it->second->cost;
        list_.erase(it->second);
        map_.erase(it);
        return true;
    }

    template <class Pred>
    void eraseIf(Pred p) {
        for (auto it = list_.begin(); it != list_.end();) {
            if (p(it->key, it->value)) {
                cost_ -= it->cost;
                map_.erase(it->key);
                it = list_.erase(it);
            } else ++it;
        }
    }

    void clear() {
        list_.clear();
        map_.clear();
        cost_ = 0;
    }

    template <class F>
    void forEach(F f) {
        for (auto& e : list_) f(e.key, e.value);
    }

private:
    struct Entry {
        K key;
        V value;
        size_t cost;
    };
    void trim() {
        while (budget_ && cost_ > budget_ && list_.size() > 1) {
            Entry& e = list_.back();
            if (evict_) evict_(e.key, e.value);
            cost_ -= e.cost;
            map_.erase(e.key);
            list_.pop_back();
        }
    }
    size_t budget_ = 0, cost_ = 0;
    EvictFn evict_;
    std::list<Entry> list_;
    std::unordered_map<K, typename std::list<Entry>::iterator, Hash> map_;
};

} // namespace cut
