#include "sstable.h"
#include "cache.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstring>
#include <iostream>

// Static assertion: 4KB blocks are safe for O_DIRECT (requires 512-byte alignment)
static_assert(cascade::BLOCK_SIZE % 512 == 0,
    "BLOCK_SIZE must be 512-byte aligned for O_DIRECT compatibility");

namespace cascade {

// ---------------------------------------------------------------------------
// Global direct-I/O flag — set by benchmark harness before engine construction.
// When true: macOS applies F_NOCACHE, Linux applies O_DIRECT|O_SYNC.
// Not thread-safe to toggle mid-run; set once before any SSTable::open() calls.
// ---------------------------------------------------------------------------
bool g_sstable_direct_io = false;


std::atomic<uint64_t> SSTable::next_id_{1};

static bool parseEntry(const uint8_t*& ptr, const uint8_t* end, KVPair& out) {
    if (ptr + sizeof(Key) + sizeof(uint32_t) + 1 > end) return false;
    std::memcpy(&out.key, ptr, sizeof(Key)); ptr += sizeof(Key);
    uint32_t vlen = 0;
    std::memcpy(&vlen, ptr, sizeof(uint32_t)); ptr += sizeof(uint32_t);
    if (ptr + vlen + 1 > end) return false;
    out.value.assign(reinterpret_cast<const char*>(ptr), vlen); ptr += vlen;
    uint8_t tomb = 0;
    std::memcpy(&tomb, ptr, 1); ptr += 1;
    out.is_tombstone = (tomb != 0);
    return true;
}

SSTable::SSTable(uint64_t id_, std::string path_, int fd_)
    : id(id_), filepath(std::move(path_)), fd_(fd_) {}

SSTable::~SSTable() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// Deprecated in-memory constructor: writes out a real temp file so behavior is consistent
SSTable::SSTable(uint64_t id_, std::vector<KVPair> sorted_data, int bloom_bits)
    : id(id_), filter(bloom_bits, optimalBloomK(14))
{
    std::atomic<int64_t> bytes_dummy{0};
    std::string path = "./data/sst_compat_" + std::to_string(id_) + ".sst";
    auto real_sst = SSTableBuilder::build(path, id_, sorted_data, bloom_bits, bytes_dummy);
    if (real_sst) {
        filepath = real_sst->filepath;
        fd_ = real_sst->fd_;
        real_sst->fd_ = -1; // transfer fd ownership
        min_key = real_sst->min_key;
        max_key = real_sst->max_key;
        file_size = real_sst->file_size;
        num_entries = real_sst->num_entries;
        index = std::move(real_sst->index);
        filter = std::move(real_sst->filter);
    }
}

std::shared_ptr<SSTable> SSTable::open(uint64_t id, const std::string& filepath) {
#if defined(__linux__)
    int open_flags = O_RDONLY | (g_sstable_direct_io ? (O_DIRECT | O_SYNC) : 0);
#else
    int open_flags = O_RDONLY;
#endif
    int fd = ::open(filepath.c_str(), open_flags);
    if (fd < 0) return nullptr;

#if defined(__APPLE__) || defined(__MACH__)
    // macOS: F_NOCACHE disables page cache for this file descriptor
    if (g_sstable_direct_io) {
        ::fcntl(fd, F_NOCACHE, 1);
    }
#endif

    off_t file_size = ::lseek(fd, 0, SEEK_END);
    if (file_size < (off_t)sizeof(SSTableFooter)) {
        ::close(fd);
        return nullptr;
    }

    SSTableFooter footer;
    ssize_t r = ::pread(fd, &footer, sizeof(footer), file_size - sizeof(footer));
    if (r != (ssize_t)sizeof(footer) || footer.magic != SSTABLE_MAGIC) {
        ::close(fd);
        return nullptr;
    }

    auto sst = std::make_shared<SSTable>(id, filepath, fd);
    sst->file_size = static_cast<uint64_t>(file_size);
    sst->num_entries = footer.num_entries;

    // Read index block
    if (footer.filter_offset > footer.index_offset) {
        size_t index_bytes = footer.filter_offset - footer.index_offset;
        size_t num_indices = index_bytes / sizeof(IndexEntry);
        sst->index.resize(num_indices);
        if (num_indices > 0) {
            ::pread(fd, sst->index.data(), index_bytes, footer.index_offset);
            sst->min_key = sst->index.front().min_key;
            sst->max_key = sst->index.back().max_key;
        }
    }

    // Read filter block
    if (file_size - (off_t)sizeof(footer) > (off_t)footer.filter_offset) {
        size_t filter_bytes = file_size - sizeof(footer) - footer.filter_offset;
        std::vector<uint8_t> fbuf(filter_bytes);
        ::pread(fd, fbuf.data(), filter_bytes, footer.filter_offset);
        sst->filter = BlockedBloomFilter::deserialize(fbuf.data(), fbuf.size());
    }

    return sst;
}

bool SSTable::search(Key key, Value& out) {
    bool is_tombstone = false;
    bool found = search(key, out, is_tombstone, nullptr);
    return found && !is_tombstone;
}

bool SSTable::search(Key key, Value& out, bool& is_tombstone, BlockCache* cache) {
    access_count.fetch_add(1, std::memory_order_relaxed);

    if (num_entries == 0) return false;
    if (key < min_key || key > max_key) return false;

    // Bloom filter gate
    if (!filter.possiblyContains(key)) return false;

    // Binary search on index entries for candidate data block
    int lo = 0, hi = (int)index.size() - 1;
    int candidate = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (index[mid].min_key <= key && key <= index[mid].max_key) {
            candidate = mid;
            break;
        } else if (key < index[mid].min_key) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    if (candidate == -1) return false;

    // Load candidate block from cache or disk
    std::vector<uint8_t> block_buf;
    uint64_t blk_off = index[candidate].block_offset;
    uint32_t blk_sz  = index[candidate].block_size;

    bool from_cache = false;
    if (cache && cache->get(id, blk_off, block_buf)) {
        from_cache = true;
    }
    if (!from_cache) {
        block_buf.resize(blk_sz);
        ssize_t r = ::pread(fd_, block_buf.data(), blk_sz, blk_off);
        if (r != (ssize_t)blk_sz) return false;
        if (cache) cache->put(id, blk_off, block_buf);
    }

    // Scan entries within this block
    const uint8_t* ptr = block_buf.data();
    const uint8_t* end = block_buf.data() + block_buf.size();
    KVPair entry;
    while (parseEntry(ptr, end, entry)) {
        if (entry.key == key) {
            is_tombstone = entry.is_tombstone;
            if (!is_tombstone) out = std::move(entry.value);
            return true;
        }
        if (entry.key > key) break;
    }

    return false;
}

std::vector<KVPair> SSTable::scan(Key start, Key end, BlockCache* cache) {
    access_count.fetch_add(1, std::memory_order_relaxed);
    std::vector<KVPair> result;
    if (num_entries == 0 || start > max_key || end < min_key) return result;

    for (const auto& ie : index) {
        if (ie.max_key < start || ie.min_key > end) continue;

        std::vector<uint8_t> block_buf;
        bool from_cache = false;
        if (cache && cache->get(id, ie.block_offset, block_buf)) {
            from_cache = true;
        }
        if (!from_cache) {
            block_buf.resize(ie.block_size);
            ssize_t r = ::pread(fd_, block_buf.data(), ie.block_size, ie.block_offset);
            if (r != (ssize_t)ie.block_size) continue;
            if (cache) cache->put(id, ie.block_offset, block_buf);
        }

        const uint8_t* ptr = block_buf.data();
        const uint8_t* block_end = block_buf.data() + block_buf.size();
        KVPair entry;
        while (parseEntry(ptr, block_end, entry)) {
            if (entry.key >= start && entry.key <= end) {
                if (!entry.is_tombstone) result.push_back(std::move(entry));
            } else if (entry.key > end) {
                break;
            }
        }
    }
    return result;
}

std::vector<KVPair> SSTable::readAll() {
    std::vector<KVPair> result;
    result.reserve(num_entries);

    for (const auto& ie : index) {
        std::vector<uint8_t> block_buf(ie.block_size);
        ssize_t r = ::pread(fd_, block_buf.data(), ie.block_size, ie.block_offset);
        if (r != (ssize_t)ie.block_size) continue;

        const uint8_t* ptr = block_buf.data();
        const uint8_t* block_end = block_buf.data() + block_buf.size();
        KVPair entry;
        while (parseEntry(ptr, block_end, entry)) {
            result.push_back(std::move(entry));
        }
    }
    return result;
}

void SSTable::removeFile() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (!filepath.empty()) {
        ::unlink(filepath.c_str());
    }
}

std::shared_ptr<SSTable> SSTableBuilder::build(
    const std::string& filepath,
    uint64_t id,
    const std::vector<KVPair>& sorted_run,
    int bloom_bits,
    std::atomic<int64_t>& bytes_written_counter,
    int bloom_k)
{
    // Ensure parent directory exists
    std::string dir = filepath.substr(0, filepath.find_last_of('/'));
    if (!dir.empty() && dir != filepath) {
        struct stat st;
        if (::stat(dir.c_str(), &st) != 0) {
            ::mkdir(dir.c_str(), 0755);
        }
    }

    int fd = ::open(filepath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::cerr << "Failed to create SSTable file: " << filepath << "\n";
        return nullptr;
    }

    BlockedBloomFilter filter(bloom_bits, bloom_k);
    for (const auto& kv : sorted_run) {
        filter.add(kv.key);
    }

    std::vector<IndexEntry> index;
    uint64_t current_offset = 0;
    std::vector<uint8_t> current_block;
    Key block_min_key = 0, block_max_key = 0;

    for (const auto& kv : sorted_run) {
        uint32_t vlen = static_cast<uint32_t>(kv.value.size());
        size_t entry_size = sizeof(Key) + sizeof(uint32_t) + vlen + 1;

        if (!current_block.empty() && current_block.size() + entry_size > BLOCK_SIZE) {
            IndexEntry ie;
            ie.min_key = block_min_key;
            ie.max_key = block_max_key;
            ie.block_offset = current_offset;
            ie.block_size = static_cast<uint32_t>(current_block.size());
            index.push_back(ie);

            ssize_t w = ::write(fd, current_block.data(), current_block.size());
            (void)w;
            current_offset += current_block.size();
            current_block.clear();
        }

        if (current_block.empty()) {
            block_min_key = kv.key;
        }
        block_max_key = kv.key;

        size_t old_sz = current_block.size();
        current_block.resize(old_sz + entry_size);
        uint8_t* ptr = current_block.data() + old_sz;

        std::memcpy(ptr, &kv.key, sizeof(Key)); ptr += sizeof(Key);
        std::memcpy(ptr, &vlen, sizeof(uint32_t)); ptr += sizeof(uint32_t);
        if (vlen > 0) {
            std::memcpy(ptr, kv.value.data(), vlen); ptr += vlen;
        }
        uint8_t tomb = kv.is_tombstone ? 1 : 0;
        std::memcpy(ptr, &tomb, 1);
    }

    if (!current_block.empty()) {
        IndexEntry ie;
        ie.min_key = block_min_key;
        ie.max_key = block_max_key;
        ie.block_offset = current_offset;
        ie.block_size = static_cast<uint32_t>(current_block.size());
        index.push_back(ie);

        ssize_t w = ::write(fd, current_block.data(), current_block.size());
        (void)w;
        current_offset += current_block.size();
        current_block.clear();
    }

    // Index block
    uint64_t index_offset = current_offset;
    if (!index.empty()) {
        size_t index_bytes = index.size() * sizeof(IndexEntry);
        ssize_t w = ::write(fd, index.data(), index_bytes);
        (void)w;
        current_offset += index_bytes;
    }

    // Filter block
    uint64_t filter_offset = current_offset;
    auto filter_bytes = filter.serialize();
    if (!filter_bytes.empty()) {
        ssize_t w = ::write(fd, filter_bytes.data(), filter_bytes.size());
        (void)w;
        current_offset += filter_bytes.size();
    }

    // Footer
    SSTableFooter footer;
    footer.index_offset = index_offset;
    footer.filter_offset = filter_offset;
    footer.num_entries = sorted_run.size();
    footer.magic = SSTABLE_MAGIC;
    ssize_t w = ::write(fd, &footer, sizeof(footer));
    (void)w;
    current_offset += sizeof(footer);

    // Plain fsync as mandated by ground rules for macOS
    ::fsync(fd);
    ::close(fd);

    bytes_written_counter.fetch_add((int64_t)current_offset, std::memory_order_relaxed);

    return SSTable::open(id, filepath);
}

std::shared_ptr<SSTable> SSTableBuilder::build(
    std::vector<KVPair> sorted_run,
    int bloom_bits,
    std::atomic<int64_t>& bytes_written_counter)
{
    uint64_t id = SSTable::next_id_++;
    std::string path = "./data/sst_" + std::to_string(id) + ".sst";
    return build(path, id, sorted_run, bloom_bits, bytes_written_counter);
}

} // namespace cascade
