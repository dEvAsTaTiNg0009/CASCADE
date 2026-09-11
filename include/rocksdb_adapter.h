#pragma once
// =============================================================================
// rocksdb_adapter.h — Thin RocksDB adapter matching CASCADE's benchmark interface
//
// Provides `RocksDBAdapter` which wraps rocksdb::DB* behind the same
// insert/search/scan/del interface used by LSMEngine, so the benchmark harness
// can call RocksDB through an identical code path.
//
// Compiled only when ROCKSDB_AVAILABLE is defined (see Makefile).
// When not available, RocksDBAdapter is a no-op stub that prints a warning.
//
// Configuration (matched to CASCADE's baseline as closely as possible):
//   write_buffer_size      = 4096 * 72  ≈ memtable_capacity × bytes_per_kv
//   max_write_buffer_number = 2
//   level0_file_num_compaction_trigger = 4
//   bloom_filter_bits_per_key = 10      (same as CASCADE's original bloom_bits_per_key)
//   block_cache_size          = 64MB    (matches CASCADE's block_cache_capacity)
//   compression               = kNoCompression  (isolates I/O from compute)
//   max_background_compactions = 1      (matches CASCADE's single bg thread)
//   max_background_flushes    = 1
//
// All config knobs documented here and in CONFIG_DETAILS.md.
// Note: This is "out of the box plus obvious tuning" — deeper RocksDB tuning
// is Phase II scope, as noted in the methods text.
// =============================================================================

#ifdef ROCKSDB_AVAILABLE

#include "common.h"
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/table.h>
#include <rocksdb/cache.h>
#include <rocksdb/slice.h>
#include <string>
#include <vector>
#include <iostream>
#include <cstdlib>
#include <memory>
#include <cstring>

namespace cascade {

// Configuration documentation struct — mirrors Config for the paper
struct RocksDBConfig {
    // memtable: 4096 keys × 72 bytes/kv = 294912 bytes ≈ 288 KB
    size_t write_buffer_size               = 4096 * 72;
    int    max_write_buffer_number         = 2;
    // leveling compaction (matches CASCADE baseline's fixed-leveling mode)
    rocksdb::CompactionStyle compaction    = rocksdb::kCompactionStyleLevel;
    int    level0_file_num_compaction_trigger = 4;
    int    max_background_compactions      = 1; // matches CASCADE single bg thread
    int    max_background_flushes          = 1;
    // bloom filter: 10 bits/key — same as CASCADE's bloom_bits_per_key config
    int    bloom_bits                      = 10;
    // block cache: 64MB — matches CASCADE's block_cache_capacity
    size_t block_cache_size                = 64ULL * 1024 * 1024;
    // compression: none — isolates I/O from compute cost
    rocksdb::CompressionType compression   = rocksdb::kNoCompression;
};

class RocksDBAdapter {
public:
    explicit RocksDBAdapter(const std::string& db_path,
                             const RocksDBConfig& rdb_cfg = RocksDBConfig())
        : db_path_(db_path)
    {
        rocksdb::BlockBasedTableOptions table_opts;
        table_opts.filter_policy.reset(
            rocksdb::NewBloomFilterPolicy(rdb_cfg.bloom_bits, false));
        table_opts.block_cache = rocksdb::NewLRUCache(rdb_cfg.block_cache_size);
        table_opts.block_size  = 4096; // 4KB blocks — matches CASCADE SSTable BLOCK_SIZE

        rocksdb::Options opts;
        opts.create_if_missing           = true;
        opts.write_buffer_size           = rdb_cfg.write_buffer_size;
        opts.max_write_buffer_number     = rdb_cfg.max_write_buffer_number;
        opts.compaction_style            = rdb_cfg.compaction;
        opts.level0_file_num_compaction_trigger = rdb_cfg.level0_file_num_compaction_trigger;
        opts.max_background_compactions  = rdb_cfg.max_background_compactions;
        opts.max_background_flushes      = rdb_cfg.max_background_flushes;
        opts.compression                 = rdb_cfg.compression;
        opts.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_opts));

        auto status = rocksdb::DB::Open(opts, db_path, &db_);
        if (!status.ok()) {
            std::cerr << "  [RocksDB] Failed to open: " << status.ToString() << "\n";
            db_.reset();
        }
    }

    ~RocksDBAdapter() {
        db_.reset();
    }

    bool ok() const { return db_ != nullptr; }

    void insert(Key key, const Value& value) {
        if (!db_) return;
        rocksdb::WriteOptions wo;
        wo.disableWAL = false; // keep WAL for fair comparison
        std::string key_str(reinterpret_cast<const char*>(&key), sizeof(Key));
        db_->Put(wo, key_str, value);
    }

    bool search(Key key, Value& out) {
        if (!db_) return false;
        rocksdb::ReadOptions ro;
        std::string key_str(reinterpret_cast<const char*>(&key), sizeof(Key));
        auto status = db_->Get(ro, key_str, &out);
        return status.ok();
    }

    void del(Key key) {
        if (!db_) return;
        rocksdb::WriteOptions wo;
        std::string key_str(reinterpret_cast<const char*>(&key), sizeof(Key));
        db_->Delete(wo, key_str);
    }

    std::vector<KVPair> scan(Key start, Key end) {
        std::vector<KVPair> result;
        if (!db_) return result;
        rocksdb::ReadOptions ro;
        std::string start_str(reinterpret_cast<const char*>(&start), sizeof(Key));
        std::string end_str(reinterpret_cast<const char*>(&end), sizeof(Key));
        rocksdb::Slice end_slice(end_str);
        ro.iterate_upper_bound = &end_slice;

        auto* it = db_->NewIterator(ro);
        for (it->Seek(start_str); it->Valid(); it->Next()) {
            Key k;
            std::memcpy(&k, it->key().data(), sizeof(Key));
            if (k > end) break;
            result.push_back({k, it->value().ToString(), false});
        }
        delete it;
        return result;
    }

    void flush() {
        if (!db_) return;
        rocksdb::FlushOptions fo;
        fo.wait = true;
        db_->Flush(fo);
    }

    // RocksDB does not expose WAF/RAF directly — return sentinel values
    double waf() const { return -1.0; }  // Not directly accessible without RocksDB stats
    double raf() const { return -1.0; }

    // Get stats string for reporting
    std::string getStats() const {
        if (!db_) return "N/A";
        std::string stats;
        db_->GetProperty("rocksdb.stats", &stats);
        return stats;
    }

    static void destroyDB(const std::string& path) {
        rocksdb::DestroyDB(path, rocksdb::Options{});
    }

private:
    std::unique_ptr<rocksdb::DB> db_;
    std::string                  db_path_;
};

} // namespace cascade

#else // ROCKSDB_AVAILABLE not defined

#include "common.h"
#include <string>
#include <vector>
#include <iostream>

namespace cascade {

// Stub — prints a clear warning when RocksDB is not compiled in
class RocksDBAdapter {
public:
    explicit RocksDBAdapter(const std::string&, ...) {
        std::cerr << "\n  [RocksDB] Not compiled in. Install RocksDB and rebuild with:\n";
        std::cerr << "    brew install rocksdb   (macOS)\n";
        std::cerr << "    sudo apt install librocksdb-dev  (Ubuntu)\n";
        std::cerr << "  Then: make rocksdb_bench\n\n";
    }
    bool ok() const { return false; }
    void insert(Key, const Value&) {}
    bool search(Key, Value&) { return false; }
    void del(Key) {}
    std::vector<KVPair> scan(Key, Key) { return {}; }
    void flush() {}
    double waf() const { return -1.0; }
    double raf() const { return -1.0; }
    static void destroyDB(const std::string&) {}
};

} // namespace cascade

#endif // ROCKSDB_AVAILABLE
