#include "cache.h"

namespace cascade {

BlockCache::BlockCache(size_t cap_bytes) : capacity_(cap_bytes) {}

bool BlockCache::get(uint64_t ssid, uint64_t offset, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find({ssid, offset});
    if (it == map_.end()) { misses_++; return false; }
    list_.splice(list_.begin(), list_, it->second);
    out = it->second->second.data;
    hits_++;
    return true;
}

void BlockCache::put(uint64_t ssid, uint64_t offset, std::vector<uint8_t> block) {
    std::lock_guard<std::mutex> lk(mu_);
    Key2 key{ssid, offset};
    size_t sz = block.size();
    auto it = map_.find(key);
    if (it != map_.end()) {
        used_ -= it->second->second.data.size();
        list_.erase(it->second);
        map_.erase(it);
    }
    list_.push_front({key, {std::move(block)}});
    map_[key] = list_.begin();
    used_ += sz;
    while (used_ > capacity_ && list_.size() > 1) {
        used_ -= list_.back().second.data.size();
        map_.erase(list_.back().first);
        list_.pop_back();
    }
}

double BlockCache::hitRate() const {
    uint64_t t = hits_ + misses_;
    return t ? (double)hits_ / t : 0.0;
}
uint64_t BlockCache::hits()   const { return hits_; }
uint64_t BlockCache::misses() const { return misses_; }

} // namespace cascade
