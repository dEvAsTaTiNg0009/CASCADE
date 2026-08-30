#pragma once
// =============================================================================
// wal.h — Write-Ahead Log with group commit
// In this research prototype, WAL is an in-memory byte buffer that mimics
// an append-only disk log. Byte accounting is exact for WAF computation.
// =============================================================================
#include "common.h"
#include <vector>
#include <mutex>
#include <atomic>

namespace cascade {

enum class WalRecordType : uint8_t { PUT = 1, DELETE = 2, COMMIT = 3 };

struct WalRecord {
    WalRecordType type;
    Key           key;
    Value         value;
};

class WAL {
public:
    explicit WAL(int group_commit_batch = 64)
        : batch_size_(group_commit_batch) {}

    // Append a record. Calls "fsync" (here: atomic commit counter++) every batch_size_ records.
    void append(WalRecordType type, Key key, const Value& value = "") {
        std::lock_guard<std::mutex> lk(mu_);
        WalRecord rec{type, key, value};
        int record_bytes = 1 + 8 + 4 + (int)value.size(); // type + key + len + val
        total_bytes_.fetch_add(record_bytes, std::memory_order_relaxed);
        log_.push_back(rec);
        if (++pending_ >= batch_size_) {
            commits_++;
            pending_ = 0;
        }
    }

    // Force group commit
    void sync() {
        std::lock_guard<std::mutex> lk(mu_);
        if (pending_ > 0) { commits_++; pending_ = 0; }
    }

    // Replay WAL for recovery (returns all records in order)
    std::vector<WalRecord> recover() {
        std::lock_guard<std::mutex> lk(mu_);
        return log_;
    }

    void truncate() {
        std::lock_guard<std::mutex> lk(mu_);
        log_.clear();
        pending_ = 0;
    }

    int64_t totalBytes()    const { return total_bytes_.load(); }
    int     totalCommits()  const { return commits_; }
    size_t  numRecords()    const { std::lock_guard<std::mutex> lk(mu_); return log_.size(); }

private:
    mutable std::mutex       mu_;
    std::vector<WalRecord>   log_;
    int                      batch_size_;
    int                      pending_  = 0;
    int                      commits_  = 0;
    std::atomic<int64_t>     total_bytes_{0};
};

} // namespace cascade
