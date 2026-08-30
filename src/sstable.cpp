#include "sstable.h"
#include <algorithm>
#include <numeric>

namespace cascade {

uint64_t SSTable::next_id_ = 1;

SSTable::SSTable(uint64_t id_, std::vector<KVPair> sorted_data, int bloom_bits)
    : id(id_), filter(bloom_bits, 8)
{
    id = id_;
    data = std::move(sorted_data);
    if (!data.empty()) {
        min_key = data.front().key;
        max_key = data.back().key;
    }
    // Build bloom filter
    for (auto& kv : data)
        if (!kv.is_tombstone)
            filter.add(kv.key);
    buildIndex();
    computeFileSize();
}

void SSTable::buildIndex() {
    index.clear();
    if (data.empty()) return;
    // Simulate 4KB data blocks
    int entries_per_block = BLOCK_SIZE / (8 + 4 + 64 + 1); // approx
    if (entries_per_block < 1) entries_per_block = 1;
    uint64_t offset = 0;
    for (int i = 0; i < (int)data.size(); i += entries_per_block) {
        int end = std::min(i + entries_per_block, (int)data.size());
        IndexEntry ie;
        ie.min_key = data[i].key;
        ie.max_key = data[end-1].key;
        ie.block_offset = offset;
        ie.block_size = BLOCK_SIZE;
        index.push_back(ie);
        offset += BLOCK_SIZE;
    }
}

void SSTable::computeFileSize() {
    // Data blocks
    file_size = (uint64_t)index.size() * BLOCK_SIZE;
    // Index block: 28 bytes per entry
    file_size += index.size() * 28;
    // Filter block
    file_size += (filter.totalBits() + 7) / 8;
    // Footer
    file_size += 32;
}

bool SSTable::search(Key key, Value& out) {
    access_count.fetch_add(1, std::memory_order_relaxed);

    // Bloom filter gate
    if (!filter.possiblyContains(key)) return false;

    // Fence pointer: find candidate data block
    // Binary search on index entries
    int lo = 0, hi = (int)index.size() - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (index[mid].max_key < key) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= (int)index.size()) return false;
    if (key < index[lo].min_key || key > index[lo].max_key) return false;

    // Binary search within the data block range
    int entries_per_block = BLOCK_SIZE / (8 + 4 + 64 + 1);
    if (entries_per_block < 1) entries_per_block = 1;
    int block_start = lo * entries_per_block;
    int block_end   = std::min(block_start + entries_per_block, (int)data.size());

    auto it = std::lower_bound(data.begin() + block_start,
                                data.begin() + block_end,
                                KVPair{key}, [](const KVPair& a, const KVPair& b){ return a.key < b.key; });
    if (it != data.begin() + block_end && it->key == key) {
        if (it->is_tombstone) return false;
        out = it->value;
        return true;
    }
    return false;
}

std::vector<KVPair> SSTable::scan(Key start, Key end) {
    access_count.fetch_add(1, std::memory_order_relaxed);
    std::vector<KVPair> result;
    auto it = std::lower_bound(data.begin(), data.end(), KVPair{start},
        [](const KVPair& a, const KVPair& b){ return a.key < b.key; });
    while (it != data.end() && it->key <= end) {
        if (!it->is_tombstone) result.push_back(*it);
        ++it;
    }
    return result;
}

std::shared_ptr<SSTable> SSTableBuilder::build(
    std::vector<KVPair> sorted_run,
    int bloom_bits,
    std::atomic<int64_t>& bytes_written_counter)
{
    auto ss = std::make_shared<SSTable>(SSTable::next_id_++, std::move(sorted_run), bloom_bits);
    bytes_written_counter.fetch_add((int64_t)ss->file_size, std::memory_order_relaxed);
    return ss;
}

} // namespace cascade
