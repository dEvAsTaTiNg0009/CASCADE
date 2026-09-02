#pragma once
// =============================================================================
// wal.h — Persistent Write-Ahead Log with group commit
// Append-only file on disk with 64-record batching and plain fsync().
// =============================================================================
#include "common.h"
#include <vector>
#include <mutex>
#include <atomic>
#include <string>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace cascade {

enum class WalRecordType : uint8_t { PUT = 1, DELETE = 2, COMMIT = 3 };

struct WalRecord {
    WalRecordType type;
    Key           key;
    Value         value;
};

class WAL {
public:
    explicit WAL(int group_commit_batch = 64, std::string filepath = "./data/wal.log")
        : batch_size_(group_commit_batch), filepath_(std::move(filepath))
    {
        initFile();
    }

    ~WAL() {
        std::lock_guard<std::mutex> lk(mu_);
        flushBufferAndSyncLocked();
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    void open(const std::string& filepath, int group_commit_batch = 64) {
        std::lock_guard<std::mutex> lk(mu_);
        flushBufferAndSyncLocked();
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        filepath_ = filepath;
        batch_size_ = group_commit_batch;
        initFileLocked();
    }

    // Append a record to WAL buffer. Calls plain fsync() every batch_size_ writes.
    void append(WalRecordType type, Key key, const Value& value = "") {
        std::lock_guard<std::mutex> lk(mu_);
        uint32_t vlen = static_cast<uint32_t>(value.size());
        size_t rec_bytes = 1 + sizeof(Key) + sizeof(uint32_t) + vlen;
        total_bytes_.fetch_add(rec_bytes, std::memory_order_relaxed);

        size_t old_sz = buffer_.size();
        buffer_.resize(old_sz + rec_bytes);
        uint8_t* ptr = buffer_.data() + old_sz;

        uint8_t t = static_cast<uint8_t>(type);
        std::memcpy(ptr, &t, 1); ptr += 1;
        std::memcpy(ptr, &key, sizeof(Key)); ptr += sizeof(Key);
        std::memcpy(ptr, &vlen, sizeof(uint32_t)); ptr += sizeof(uint32_t);
        if (vlen > 0) {
            std::memcpy(ptr, value.data(), vlen);
        }

        num_records_++;
        if (++pending_ >= batch_size_) {
            flushBufferAndSyncLocked();
        }
    }

    // Force group commit (syncs pending buffer to disk with plain fsync)
    void sync() {
        std::lock_guard<std::mutex> lk(mu_);
        if (pending_ > 0 || !buffer_.empty()) {
            flushBufferAndSyncLocked();
        }
    }

    // Replay WAL from disk for recovery (returns all durable records in order)
    std::vector<WalRecord> recover() {
        std::lock_guard<std::mutex> lk(mu_);
        flushBufferAndSyncLocked();

        std::vector<WalRecord> records;
        int rfd = ::open(filepath_.c_str(), O_RDONLY);
        if (rfd < 0) return records;

        off_t fsize = ::lseek(rfd, 0, SEEK_END);
        if (fsize <= 0) {
            ::close(rfd);
            return records;
        }

        std::vector<uint8_t> data(fsize);
        ssize_t r = ::pread(rfd, data.data(), fsize, 0);
        ::close(rfd);
        if (r <= 0) return records;

        const uint8_t* ptr = data.data();
        const uint8_t* end = data.data() + r;

        while (ptr + 1 + sizeof(Key) + sizeof(uint32_t) <= end) {
            uint8_t type_byte = *ptr++;
            Key k;
            std::memcpy(&k, ptr, sizeof(Key)); ptr += sizeof(Key);
            uint32_t vlen;
            std::memcpy(&vlen, ptr, sizeof(uint32_t)); ptr += sizeof(uint32_t);
            if (ptr + vlen > end) break; // Truncated/partial write at crash point
            std::string val(reinterpret_cast<const char*>(ptr), vlen); ptr += vlen;

            records.push_back({static_cast<WalRecordType>(type_byte), k, std::move(val)});
        }
        return records;
    }

    // Truncate WAL on disk after memtable has flushed to SSTable
    void truncate() {
        std::lock_guard<std::mutex> lk(mu_);
        buffer_.clear();
        pending_ = 0;
        num_records_ = 0;
        if (fd_ >= 0) {
            ::ftruncate(fd_, 0);
            ::lseek(fd_, 0, SEEK_SET);
            ::fsync(fd_);
        }
    }

    int64_t totalBytes()    const { return total_bytes_.load(); }
    int     totalCommits()  const { return commits_; }
    size_t  numRecords()    const { std::lock_guard<std::mutex> lk(mu_); return num_records_; }
    const std::string& filepath() const { return filepath_; }

private:
    mutable std::mutex       mu_;
    int                      batch_size_;
    std::string              filepath_;
    int                      fd_ = -1;
    int                      pending_     = 0;
    int                      commits_     = 0;
    size_t                   num_records_ = 0;
    std::vector<uint8_t>     buffer_;
    std::atomic<int64_t>     total_bytes_{0};

    void initFile() {
        std::lock_guard<std::mutex> lk(mu_);
        initFileLocked();
    }

    void initFileLocked() {
        if (filepath_.empty()) return;
        std::string dir = filepath_.substr(0, filepath_.find_last_of('/'));
        if (!dir.empty() && dir != filepath_) {
            struct stat st;
            if (::stat(dir.c_str(), &st) != 0) {
                ::mkdir(dir.c_str(), 0755);
            }
        }
        fd_ = ::open(filepath_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    }

    void flushBufferAndSyncLocked() {
        if (fd_ >= 0 && !buffer_.empty()) {
            ssize_t w = ::write(fd_, buffer_.data(), buffer_.size());
            (void)w;
            ::fsync(fd_); // Plain fsync as mandated by ground rules for macOS
            commits_++;
            buffer_.clear();
            pending_ = 0;
        }
    }
};

} // namespace cascade
