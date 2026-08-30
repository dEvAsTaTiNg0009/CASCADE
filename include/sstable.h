#pragma once
// =============================================================================
// sstable.h — SSTable file format (in-memory simulation)
//
// Format:
//   [DATA BLOCKS ...] [INDEX BLOCK] [FILTER BLOCK] [FOOTER]
//
// Data Block (4KB):
//   Series of KV entries with restart points for binary search.
//   Entry: [key(8B)][val_len(4B)][val_bytes][is_tombstone(1B)]
//
// Index Block:
//   One entry per data block: [min_key(8B)][max_key(8B)][offset(8B)][size(4B)]
//
// Filter Block:
//   Raw Bloom filter bits for this SSTable.
//
// Footer (32B):
//   [index_offset(8B)][filter_offset(8B)][num_entries(8B)][magic(8B)]
//
// In this research prototype, data is held in memory (no actual disk I/O)
// but byte sizes are computed exactly for WAF/RAF/SAF accounting.
// =============================================================================
#include "common.h"
#include "bloom.h"
#include <vector>
#include <cstdint>
#include <atomic>
#include <memory>

namespace cascade {

static constexpr uint64_t SSTABLE_MAGIC = 0xCA5CADE0F11EULL;
static constexpr int BLOCK_SIZE = 4096;

struct IndexEntry {
    Key      min_key;
    Key      max_key;
    uint64_t block_offset;
    uint32_t block_size;
};

class SSTable {
public:
    uint64_t id;
    Key      min_key;
    Key      max_key;
    uint64_t file_size = 0; // simulated byte size

    // All data, held in memory (simulation of disk)
    std::vector<KVPair>    data;          // sorted KV pairs
    std::vector<IndexEntry> index;        // data block index
    BlockedBloomFilter     filter;        // per-table Bloom filter

    // Access frequency for Merlin tracker
    std::atomic<int>       access_count{0};

    explicit SSTable(uint64_t id_, std::vector<KVPair> sorted_data, int bloom_bits = 8192);

    // Point lookup (uses bloom first)
    bool search(Key key, Value& out);

    // Range scan
    std::vector<KVPair> scan(Key start, Key end);

    int numEntries() const { return (int)data.size(); }

public:
    static uint64_t next_id_;
    void buildIndex();
    void computeFileSize();
};

// Builder: creates an SSTable from a sorted run
class SSTableBuilder {
public:
    static std::shared_ptr<SSTable> build(
        std::vector<KVPair> sorted_run,
        int bloom_bits,
        std::atomic<int64_t>& bytes_written_counter);
};

} // namespace cascade
