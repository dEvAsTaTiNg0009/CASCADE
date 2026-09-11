#include "csb_tree.h"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstring>
#include <chrono>

namespace cascade {

// ===========================================================================
// CSBPartition Implementation
// ===========================================================================

CSBPartition::CSBPartition() {
    root = allocGroup(1);
    root->isLeaf = true;
    height = 0;
    count.store(0, std::memory_order_relaxed);
}

CSBPartition::~CSBPartition() {
    freeAll();
}

CSBNode* CSBPartition::allocGroup(int n) {
    CSBNode* g = new CSBNode[n];
    SpinLock::Guard lk(alloc_lock);
    all_groups.push_back(g);
    return g;
}

void CSBPartition::deferFreeGroup(CSBNode* g) {
    if (!g) return;
    uint64_t epoch = EpochManager::instance().global();
    EpochManager::instance().deferFree(g, epoch, [](void* p){
        delete[] reinterpret_cast<CSBNode*>(p);
    });
    EpochManager::instance().advance();
}

void CSBPartition::freeAll() {
    SpinLock::Guard lk(alloc_lock);
    // Recursively delete allocated leaf value strings
    std::function<void(CSBNode*, int)> freeValues = [&](CSBNode* node, int depth) {
        if (!node) return;
        if (depth == 0 || node->isLeaf) {
            for (int i = 0; i < node->numKeys; i++) {
                delete node->vals[i];
                node->vals[i] = nullptr;
            }
            return;
        }
        for (int i = 0; i <= node->numKeys; i++) {
            if (node->childGroup)
                freeValues(&node->childGroup[i], depth - 1);
        }
    };
    if (root) freeValues(root, height);

    for (CSBNode* g : all_groups) {
        deferFreeGroup(g);
    }
    all_groups.clear();
    root = nullptr;
}

CSBNode* CSBPartition::findLeaf(Key key) const {
    CSBNode* node = root;
    for (int d = height; d > 0; d--) {
        if (!node) return nullptr;
        int idx = 0;
        int nk = node->numKeys;
        while (idx < nk && key >= node->keys[idx]) idx++;
        CSBNode* kids = node->childGroup;
        if (!kids) return node;
        node = &kids[idx];
    }
    return node;
}

bool CSBPartition::search(Key key, Value& out, bool& is_tombstone) {
    is_tombstone = false;
    CSBNode* leaf = findLeaf(key);
    if (!leaf) return false;
    for (int i = 0; i < leaf->numKeys; i++) {
        if (leaf->keys[i] == key) {
            if (!leaf->vals[i]) {
                is_tombstone = true;
            } else {
                out = *leaf->vals[i];
            }
            return true;
        }
    }
    return false;
}

void CSBPartition::insertLeaf(CSBNode* leaf, Key key, const Value& value) {
    int pos = 0;
    while (pos < leaf->numKeys && leaf->keys[pos] < key) pos++;

    if (pos < leaf->numKeys && leaf->keys[pos] == key) {
        delete leaf->vals[pos];
        leaf->vals[pos] = (value.empty() ? nullptr : new Value(value));
        return;
    }
    for (int i = leaf->numKeys; i > pos; i--) {
        leaf->keys[i] = leaf->keys[i-1];
        leaf->vals[i] = leaf->vals[i-1];
    }
    leaf->keys[pos] = key;
    leaf->vals[pos] = (value.empty() ? nullptr : new Value(value));
    leaf->numKeys++;
}

bool CSBPartition::insert(Key key, const Value& value) {
    // 1. Root full -> root split (height increases)
    if (root->numKeys >= CSB_ORDER) {
        int mid = root->numKeys / 2;
        Key sep = root->keys[mid];
        bool rootIsLeaf = (height == 0 || root->isLeaf);

        CSBNode* newLeaves = allocGroup(2);
        newLeaves[0].isLeaf = rootIsLeaf;
        newLeaves[1].isLeaf = rootIsLeaf;

        for (int i = 0; i < mid; i++) {
            newLeaves[0].keys[i] = root->keys[i];
            if (rootIsLeaf) newLeaves[0].vals[i] = root->vals[i];
        }
        newLeaves[0].numKeys = mid;
        if (!rootIsLeaf) {
            int leftChildren = mid + 1;
            CSBNode* leftKids = allocGroup(leftChildren);
            for (int i = 0; i < leftChildren; i++) leftKids[i] = root->childGroup[i];
            newLeaves[0].childGroup = leftKids;
        }

        int rstart = rootIsLeaf ? mid : mid + 1;
        int rcount = root->numKeys - rstart;
        for (int i = 0; i < rcount; i++) {
            newLeaves[1].keys[i] = root->keys[rstart + i];
            if (rootIsLeaf) newLeaves[1].vals[i] = root->vals[rstart + i];
        }
        newLeaves[1].numKeys = rcount;
        if (!rootIsLeaf) {
            int rightChildren = root->numKeys + 1 - (mid + 1);
            CSBNode* rightKids = allocGroup(rightChildren);
            for (int i = 0; i < rightChildren; i++)
                rightKids[i] = root->childGroup[mid + 1 + i];
            newLeaves[1].childGroup = rightKids;
        }

        CSBNode* newRoot = allocGroup(1);
        newRoot[0].isLeaf = false;
        newRoot[0].keys[0] = sep;
        newRoot[0].numKeys = 1;
        newRoot[0].childGroup = newLeaves;

        root = &newRoot[0];
        height++;
    }

    // 2. Proactive split descent
    bool isNew = true;
    std::function<void(CSBNode*, Key, const Value*, int)> insertDescend;
    insertDescend = [&](CSBNode* node, Key k, const Value* v, int depth) {
        if (depth == 0 || node->isLeaf) {
            for (int i = 0; i < node->numKeys; i++) {
                if (node->keys[i] == k) { isNew = false; break; }
            }
            insertLeaf(node, k, v ? *v : "");
            if (isNew) count.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        int idx = 0;
        while (idx < node->numKeys && k >= node->keys[idx]) idx++;
        CSBNode* child = &node->childGroup[idx];
        int numChildren = node->numKeys + 1;
        bool childIsLeaf = (depth == 1 || child->isLeaf);

        if (child->numKeys >= CSB_ORDER) {
            int mid = child->numKeys / 2;
            Key sep = child->keys[mid];

            int newCount = numChildren + 1;
            CSBNode* newGroup = allocGroup(newCount);

            for (int i = 0; i < idx; i++) newGroup[i] = node->childGroup[i];

            newGroup[idx] = CSBNode();
            newGroup[idx].isLeaf = childIsLeaf;
            newGroup[idx].numKeys = mid;
            for (int i = 0; i < mid; i++) {
                newGroup[idx].keys[i] = child->keys[i];
                if (childIsLeaf) newGroup[idx].vals[i] = child->vals[i];
            }
            if (!childIsLeaf) {
                int leftGC = mid + 1;
                CSBNode* leftKids = allocGroup(leftGC);
                for (int i = 0; i < leftGC; i++) leftKids[i] = child->childGroup[i];
                newGroup[idx].childGroup = leftKids;
            }

            int rstart = childIsLeaf ? mid : mid + 1;
            int rcount = child->numKeys - rstart;
            newGroup[idx+1] = CSBNode();
            newGroup[idx+1].isLeaf = childIsLeaf;
            newGroup[idx+1].numKeys = rcount;
            for (int i = 0; i < rcount; i++) {
                newGroup[idx+1].keys[i] = child->keys[rstart + i];
                if (childIsLeaf) newGroup[idx+1].vals[i] = child->vals[rstart + i];
            }
            if (!childIsLeaf) {
                int rightGC = child->numKeys + 1 - (mid + 1);
                CSBNode* rightKids = allocGroup(rightGC);
                for (int i = 0; i < rightGC; i++)
                    rightKids[i] = child->childGroup[mid + 1 + i];
                newGroup[idx+1].childGroup = rightKids;
            }

            for (int i = idx + 1; i < numChildren; i++) newGroup[i+1] = node->childGroup[i];

            for (int i = node->numKeys; i > idx; i--) node->keys[i] = node->keys[i-1];
            node->keys[idx] = sep;
            node->numKeys++;
            node->childGroup = newGroup;

            if (k >= sep) idx++;
            child = &node->childGroup[idx];
        }

        insertDescend(child, k, v, depth - 1);
    };

    const Value* vp = value.empty() ? nullptr : &value;
    insertDescend(root, key, vp, height);

    EpochManager::instance().runGC();
    return isNew;
}

void CSBPartition::scan(Key start, Key end, std::vector<KVPair>& out) {
    std::function<void(CSBNode*, int)> walk = [&](CSBNode* node, int depth) {
        if (!node) return;
        if (depth == 0 || node->isLeaf) {
            for (int i = 0; i < node->numKeys; i++) {
                if (node->keys[i] >= start && node->keys[i] <= end) {
                    KVPair kv;
                    kv.key = node->keys[i];
                    kv.is_tombstone = (node->vals[i] == nullptr);
                    if (!kv.is_tombstone) kv.value = *node->vals[i];
                    out.push_back(kv);
                }
            }
            return;
        }
        for (int i = 0; i <= node->numKeys; i++) {
            if (node->childGroup)
                walk(&node->childGroup[i], depth - 1);
        }
    };
    walk(root, height);
}

void CSBPartition::collectAll(CSBNode* node, int depth, std::vector<KVPair>& out) const {
    if (!node) return;
    if (depth == 0 || node->isLeaf) {
        for (int i = 0; i < node->numKeys; i++) {
            KVPair kv;
            kv.key = node->keys[i];
            kv.is_tombstone = (node->vals[i] == nullptr);
            if (!kv.is_tombstone) kv.value = *node->vals[i];
            out.push_back(kv);
        }
        return;
    }
    for (int i = 0; i <= node->numKeys; i++) {
        if (node->childGroup)
            collectAll(&node->childGroup[i], depth - 1, out);
    }
}

void CSBPartition::flush(std::vector<KVPair>& out) {
    collectAll(root, height, out);
    freeAll();
    root = allocGroup(1);
    root->isLeaf = true;
    height = 0;
    count.store(0, std::memory_order_relaxed);
}

// ===========================================================================
// ConcurrentCSBTree Implementation
// ===========================================================================

ConcurrentCSBTree::ConcurrentCSBTree() {
    partitions_ = std::make_unique<CSBPartition[]>(NUM_PARTITIONS);
    total_count_.store(0, std::memory_order_relaxed);
}

ConcurrentCSBTree::~ConcurrentCSBTree() = default;

void ConcurrentCSBTree::insert(Key key, const Value& value) {
    size_t p = getPartition(key);
    auto& part = partitions_[p];

    auto t0 = std::chrono::high_resolution_clock::now();
    part.rw_mu.lock();
    auto t1 = std::chrono::high_resolution_clock::now();
    total_lock_wait_ns_.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count(),
        std::memory_order_relaxed);
    total_lock_acquisitions_.fetch_add(1, std::memory_order_relaxed);

    std::unique_lock<std::shared_mutex> lk(part.rw_mu, std::adopt_lock);
    bool isNew = part.insert(key, value);
    if (isNew) {
        total_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool ConcurrentCSBTree::search(Key key, Value& out_value, bool& is_tombstone) {
    size_t p = getPartition(key);
    auto& part = partitions_[p];
    std::shared_lock<std::shared_mutex> lk(part.rw_mu);
    return part.search(key, out_value, is_tombstone);
}

void ConcurrentCSBTree::del(Key key) {
    insert(key, "");
}

std::vector<KVPair> ConcurrentCSBTree::scan(Key start, Key end) {
    std::vector<KVPair> result;
    for (size_t i = 0; i < NUM_PARTITIONS; i++) {
        std::shared_lock<std::shared_mutex> lk(partitions_[i].rw_mu);
        partitions_[i].scan(start, end, result);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<KVPair> ConcurrentCSBTree::flush() {
    // Acquire exclusive locks across all partitions in canonical index order to prevent deadlocks
    std::vector<std::unique_lock<std::shared_mutex>> locks;
    locks.reserve(NUM_PARTITIONS);
    for (size_t i = 0; i < NUM_PARTITIONS; i++) {
        locks.emplace_back(partitions_[i].rw_mu);
    }

    std::vector<KVPair> result;
    result.reserve(total_count_.load(std::memory_order_relaxed));
    for (size_t i = 0; i < NUM_PARTITIONS; i++) {
        partitions_[i].flush(result);
    }
    std::sort(result.begin(), result.end());
    total_count_.store(0, std::memory_order_relaxed);

    EpochManager::instance().runGC();
    return result;
}

bool ConcurrentCSBTree::isFull(int cap) const {
    return total_count_.load(std::memory_order_relaxed) >= cap;
}

int ConcurrentCSBTree::size() const {
    return total_count_.load(std::memory_order_relaxed);
}

} // namespace cascade
