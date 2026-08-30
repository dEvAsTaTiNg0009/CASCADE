#pragma once
// =============================================================================
// epoch.h — Epoch-Based Memory Reclamation (EBMM)
// Ensures split/retired CSB+ node groups are not freed while any reader
// thread is still traversing them. Based on the Silo EBMM protocol.
// =============================================================================
#include <atomic>
#include <vector>
#include <thread>
#include <mutex>
#include <functional>
#include <cstdint>

namespace cascade {

static constexpr uint64_t EPOCH_INACTIVE = std::numeric_limits<uint64_t>::max();

class EpochManager {
public:
    static EpochManager& instance() {
        static EpochManager em;
        return em;
    }

    // Advance the global epoch (called periodically or on compaction)
    void advance() { global_epoch_.fetch_add(1, std::memory_order_seq_cst); }

    uint64_t global() const { return global_epoch_.load(std::memory_order_acquire); }

    // Register/unregister a thread epoch slot
    uint64_t* registerThread() {
        std::lock_guard<std::mutex> lk(mu_);
        thread_epochs_.push_back(std::make_unique<std::atomic<uint64_t>>(EPOCH_INACTIVE));
        return reinterpret_cast<uint64_t*>(thread_epochs_.back().get());
    }

    void unregisterThread(uint64_t* slot) {
        reinterpret_cast<std::atomic<uint64_t>*>(slot)->store(EPOCH_INACTIVE,
            std::memory_order_release);
    }

    // Compute safe epoch = min of all active thread epochs
    uint64_t safeEpoch() const {
        std::lock_guard<std::mutex> lk(mu_);
        uint64_t safe = global_epoch_.load(std::memory_order_acquire);
        for (auto& te : thread_epochs_) {
            uint64_t v = te->load(std::memory_order_acquire);
            if (v != EPOCH_INACTIVE && v < safe) safe = v;
        }
        return safe;
    }

    // Defer freeing a block of memory until it is safe
    void deferFree(void* ptr, uint64_t epoch, std::function<void(void*)> deleter) {
        std::lock_guard<std::mutex> lk(mu_);
        deferred_.push_back({ptr, epoch, deleter});
    }

    // Run deferred frees whose epoch is now safe
    void runGC() {
        uint64_t safe = safeEpoch();
        std::lock_guard<std::mutex> lk(mu_);
        auto it = deferred_.begin();
        while (it != deferred_.end()) {
            if (it->epoch < safe) {
                it->deleter(it->ptr);
                it = deferred_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    EpochManager() : global_epoch_(0) {}

    std::atomic<uint64_t> global_epoch_;
    mutable std::mutex mu_;
    std::vector<std::unique_ptr<std::atomic<uint64_t>>> thread_epochs_;
    struct DeferEntry {
        void* ptr;
        uint64_t epoch;
        std::function<void(void*)> deleter;
    };
    std::vector<DeferEntry> deferred_;
};

// RAII epoch guard — thread pins current epoch on construction, releases on destruction
class EpochGuard {
public:
    EpochGuard() {
        static thread_local uint64_t* slot = EpochManager::instance().registerThread();
        slot_ = slot;
        pin();
    }
    ~EpochGuard() {
        unpin();
    }
    void pin() {
        if (slot_)
            reinterpret_cast<std::atomic<uint64_t>*>(slot_)->store(
                EpochManager::instance().global(), std::memory_order_release);
    }
    void unpin() {
        if (slot_)
            reinterpret_cast<std::atomic<uint64_t>*>(slot_)->store(
                EPOCH_INACTIVE, std::memory_order_release);
    }
private:
    uint64_t* slot_ = nullptr;
};

} // namespace cascade
