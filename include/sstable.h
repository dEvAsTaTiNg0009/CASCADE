#pragma once
// =============================================================================
// sstable.h — Real disk file-backed SSTable format
//
// Format:
//   [DATA BLOCKS ...] [INDEX BLOCK] [FILTER BLOCK] [FOOTER]
//
// Data Block (4KB):
//   Series of KV entries.
//   Entry: [key(8B)][val_len(4B)][val_bytes][is_tombstone(1B)]
//
// Index Block:
//   One entry per data block: [min_key(8B)][max_key(8B)][offset(8B)][size(4B)]
//
// Filter Block:
//   Serialized BlockedBloomFilter bits for this SSTable.
//
// Footer (32B):
//   [index_offset(8B)][filter_offset(8B)][num_entries(8B)][magic(8B)]
// =============================================================================
#include "common.h"
#include "bloom.h"
#include <vector>
#include <cstdint>
#include <atomic>
#include <memory>
#include <string>

namespace cascade {

static constexpr uint64_t SSTABLE_MAGIC = 0xCA5CADE0F11EULL;
static constexpr int BLOCK_SIZE = 4096;

#pragma pack(push, 1)
struct IndexEntry {
    Key      min_key;
    Key      max_key;
    uint64_t block_offset;
    uint32_t block_size;
};

struct SSTableFooter {
    uint64_t index_offset;
    uint64_t filter_offset;
    uint64_t num_entries;
    uint64_t magic;
};
#pragma pack(pop)

static_assert(sizeof(IndexEntry) == 28, "IndexEntry must be 28 bytes");
static_assert(sizeof(SSTableFooter) == 32, "SSTableFooter must be 32 bytes");

class BlockCache;

class SSTable {
public:
    uint64_t id = 0;
    Key      min_key = 0;
    Key      max_key = 0;
    uint64_t file_size = 0;
    std::string filepath;

    std::vector<IndexEntry> index;
    BlockedBloomFilter     filter;
    uint64_t               num_entries = 0;

    std::atomic<int>       access_count{0};
    int                    fd_ = -1;

    SSTable(uint64_t id_, std::string path_, int fd_);
    ~SSTable();

    // Deprecated in-memory-only constructor
    [[deprecated("Use SSTableBuilder::build() for real disk-backed SSTables")]]
    explicit SSTable(uint64_t id_, std::vector<KVPair> sorted_data, int bloom_bits = 8192);

    static std::shared_ptr<SSTable> open(uint64_t id, const std::string& filepath);

    bool search(Key key, Value& out);
    bool search(Key key, Value& out, bool& is_tombstone, BlockCache* cache = nullptr);

    std::vector<KVPair> scan(Key start, Key end, BlockCache* cache = nullptr);
    std::vector<KVPair> readAll();

    void removeFile();
    int numEntries() const { return (int)num_entries; }

    static std::atomic<uint64_t> next_id_;
};

class SSTableBuilder {
public:
    static std::shared_ptr<SSTable> build(
        const std::string& filepath,
        uint64_t id,
        const std::vector<KVPair>& sorted_run,
        int bloom_bits,
        std::atomic<int64_t>& bytes_written_counter);

    // Backward-compatible overload
    static std::shared_ptr<SSTable> build(
        std::vector<KVPair> sorted_run,
        int bloom_bits,
        std::atomic<int64_t>& bytes_written_counter);
};

} // namespace cascade
