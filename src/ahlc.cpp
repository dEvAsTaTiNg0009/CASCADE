#include "ahlc.h"
#include "sstable.h"
#include <queue>

namespace cascade {

// ---------------------------------------------------------------------------
// K-way merge with tombstone deduplication
// Uses a min-heap for O((N log k) merge
// ---------------------------------------------------------------------------
std::vector<KVPair> mergeRuns(std::vector<std::vector<KVPair>>& runs,
                               bool remove_tombstones)
{
    using Entry = std::tuple<Key, int, int>; // (key, run_idx, pos)
    auto cmp = [](const Entry& a, const Entry& b) {
        if (std::get<0>(a) != std::get<0>(b))
            return std::get<0>(a) > std::get<0>(b);
        return std::get<1>(a) < std::get<1>(b); // higher run index (newer run) pops first!
    };
    std::priority_queue<Entry, std::vector<Entry>, decltype(cmp)> heap(cmp);

    for (int i = 0; i < (int)runs.size(); i++)
        if (!runs[i].empty())
            heap.push({runs[i][0].key, i, 0});

    std::vector<KVPair> result;
    Key last_key = TOMBSTONE;

    while (!heap.empty()) {
        auto [k, ri, pos] = heap.top(); heap.pop();

        if (k != last_key) {
            const KVPair& kv = runs[ri][pos];
            if (!remove_tombstones || !kv.is_tombstone) {
                result.push_back(kv);
                last_key = k;
            }
        }

        if (pos + 1 < (int)runs[ri].size())
            heap.push({runs[ri][pos+1].key, ri, pos+1});
    }
    return result;
}

// ---------------------------------------------------------------------------
// Leveling compaction
// ---------------------------------------------------------------------------
void compactLeveling(std::vector<Level>& levels, int level,
                     std::atomic<int64_t>& bytes_written)
{
    if (level < 0 || level >= (int)levels.size()) return;
    if (levels[level].empty()) return;

    int nextLevel = level + 1;
    if (nextLevel >= (int)levels.size()) levels.push_back({});

    // Collect all runs from this level + next level
    std::vector<std::vector<KVPair>> all_runs;
    for (auto& run : levels[level])   all_runs.push_back(std::move(run));
    for (auto& run : levels[nextLevel]) all_runs.push_back(std::move(run));
    levels[level].clear();
    levels[nextLevel].clear();

    auto merged = mergeRuns(all_runs, false); // preserve tombstones across levels

    // Track I/O bytes written
    bytes_written.fetch_add((int64_t)merged.size() * 72, std::memory_order_relaxed);

    levels[nextLevel].push_back(std::move(merged));
}

// ---------------------------------------------------------------------------
// Tiering compaction
// ---------------------------------------------------------------------------
void compactTiering(std::vector<Level>& levels, int level, int maxRuns,
                    std::atomic<int64_t>& bytes_written)
{
    if (level < 0 || level >= (int)levels.size()) return;

    int numRuns = (int)levels[level].size();
    int totalEntries = 0;
    for (auto& r : levels[level]) totalEntries += (int)r.size();
    bool runFull  = numRuns >= maxRuns;
    bool capFull  = numRuns > 0 && (totalEntries / numRuns) > 4000;
    if (!runFull && !capFull) return;

    int nextLevel = level + 1;
    if (nextLevel >= (int)levels.size()) levels.push_back({});

    auto merged = mergeRuns(levels[level], false);
    levels[level].clear();
    bytes_written.fetch_add((int64_t)merged.size() * 72, std::memory_order_relaxed);
    levels[nextLevel].push_back(std::move(merged));
}

// ---------------------------------------------------------------------------
// Sub-range compaction — merge only runs that overlap [lo, hi)
// This avoids full level-wide merge, reducing write-stall duration
// ---------------------------------------------------------------------------
void compactSubRange(std::vector<Level>& levels, int level,
                     Key lo, Key hi,
                     std::atomic<int64_t>& bytes_written)
{
    if (level < 0 || level >= (int)levels.size()) return;

    std::vector<std::vector<KVPair>> overlapping;
    std::vector<std::vector<KVPair>> remaining;

    for (auto& run : levels[level]) {
        if (run.empty()) continue;
        Key rmin = run.front().key, rmax = run.back().key;
        if (rmin <= hi && rmax >= lo)
            overlapping.push_back(std::move(run));
        else
            remaining.push_back(std::move(run));
    }
    levels[level] = std::move(remaining);

    if (overlapping.empty()) return;

    int nextLevel = level + 1;
    if (nextLevel >= (int)levels.size()) levels.push_back({});

    auto merged = mergeRuns(overlapping, false);
    bytes_written.fetch_add((int64_t)merged.size() * 72, std::memory_order_relaxed);
    levels[nextLevel].push_back(std::move(merged));
}

void compactLeveling(std::vector<SSTableLevel>& levels, int level,
                     const std::string& db_path,
                     std::atomic<int64_t>& bytes_written,
                     int bloom_bits_per_key)
{
    if (level < 0 || level >= (int)levels.size()) return;
    if (levels[level].empty()) return;

    int nextLevel = level + 1;
    if (nextLevel >= (int)levels.size()) levels.push_back({});

    std::vector<std::vector<KVPair>> all_runs;
    std::vector<std::shared_ptr<SSTable>> to_delete;

    for (auto& sst : levels[level]) {
        if (sst) {
            all_runs.push_back(sst->readAll());
            to_delete.push_back(sst);
        }
    }
    for (auto& sst : levels[nextLevel]) {
        if (sst) {
            all_runs.push_back(sst->readAll());
            to_delete.push_back(sst);
        }
    }
    levels[level].clear();
    levels[nextLevel].clear();

    auto merged = mergeRuns(all_runs, false);

    uint64_t id = SSTable::next_id_++;
    std::string sst_path = db_path + "/L" + std::to_string(nextLevel) + "_" + std::to_string(id) + ".sst";
    int bloom_bits = std::max(512, (int)(bloom_bits_per_key * merged.size()));
    auto new_sst = SSTableBuilder::build(sst_path, id, merged, bloom_bits,
                                         bytes_written, optimalBloomK(bloom_bits_per_key));

    for (auto& sst : to_delete) {
        sst->removeFile();
    }

    if (new_sst) {
        levels[nextLevel].push_back(new_sst);
    }
}

void compactTiering(std::vector<SSTableLevel>& levels, int level, int maxRuns,
                    const std::string& db_path,
                    std::atomic<int64_t>& bytes_written,
                    int bloom_bits_per_key)
{
    if (level < 0 || level >= (int)levels.size()) return;

    int numRuns = (int)levels[level].size();
    int totalEntries = 0;
    for (auto& sst : levels[level]) if (sst) totalEntries += sst->numEntries();
    bool runFull  = numRuns >= maxRuns;
    bool capFull  = numRuns > 0 && (totalEntries / numRuns) > 4000;
    if (!runFull && !capFull) return;

    int nextLevel = level + 1;
    if (nextLevel >= (int)levels.size()) levels.push_back({});

    std::vector<std::vector<KVPair>> all_runs;
    std::vector<std::shared_ptr<SSTable>> to_delete;
    for (auto& sst : levels[level]) {
        if (sst) {
            all_runs.push_back(sst->readAll());
            to_delete.push_back(sst);
        }
    }
    levels[level].clear();

    auto merged = mergeRuns(all_runs, false);

    uint64_t id = SSTable::next_id_++;
    std::string sst_path = db_path + "/L" + std::to_string(nextLevel) + "_" + std::to_string(id) + ".sst";
    int bloom_bits = std::max(512, (int)(bloom_bits_per_key * merged.size()));
    auto new_sst = SSTableBuilder::build(sst_path, id, merged, bloom_bits,
                                         bytes_written, optimalBloomK(bloom_bits_per_key));

    for (auto& sst : to_delete) {
        sst->removeFile();
    }

    if (new_sst) {
        levels[nextLevel].push_back(new_sst);
    }
}

void compactSubRange(std::vector<SSTableLevel>& levels, int level,
                     Key lo, Key hi,
                     const std::string& db_path,
                     std::atomic<int64_t>& bytes_written,
                     int bloom_bits_per_key)
{
    if (level < 0 || level >= (int)levels.size()) return;

    std::vector<std::vector<KVPair>> overlapping;
    std::vector<std::shared_ptr<SSTable>> to_delete;
    std::vector<std::shared_ptr<SSTable>> remaining;

    for (auto& sst : levels[level]) {
        if (!sst) continue;
        if (sst->min_key <= hi && sst->max_key >= lo) {
            overlapping.push_back(sst->readAll());
            to_delete.push_back(sst);
        } else {
            remaining.push_back(sst);
        }
    }
    levels[level] = std::move(remaining);

    if (overlapping.empty()) return;

    int nextLevel = level + 1;
    if (nextLevel >= (int)levels.size()) levels.push_back({});

    auto merged = mergeRuns(overlapping, false);

    uint64_t id = SSTable::next_id_++;
    std::string sst_path = db_path + "/L" + std::to_string(nextLevel) + "_" + std::to_string(id) + ".sst";
    int bloom_bits = std::max(512, (int)(bloom_bits_per_key * merged.size()));
    auto new_sst = SSTableBuilder::build(sst_path, id, merged, bloom_bits,
                                         bytes_written, optimalBloomK(bloom_bits_per_key));

    for (auto& sst : to_delete) {
        sst->removeFile();
    }

    if (new_sst) {
        levels[nextLevel].push_back(new_sst);
    }
}

} // namespace cascade
