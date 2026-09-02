#include "lsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <unistd.h>
#include <sys/stat.h>

using namespace cascade;

int getOpenFileDescriptors(pid_t pid) {
    std::string cmd = "lsof -p " + std::to_string(pid) + " 2>/dev/null | wc -l";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return -1;
    char buf[64];
    int count = -1;
    if (fgets(buf, sizeof(buf), fp)) {
        count = std::stoi(buf);
    }
    pclose(fp);
    return count;
}

int main() {
    std::cout << "=== Step 3: Internal Smoke Test (1,000 ops) ===\n";
    pid_t pid = getpid();
    int initial_fds = getOpenFileDescriptors(pid);
    std::cout << "Initial open file descriptors (PID " << pid << "): " << initial_fds << "\n";

    std::string db_dir = "./data_smoke";

    for (int iteration = 1; iteration <= 3; iteration++) {
        (void)system(("rm -rf " + db_dir + " && mkdir -p " + db_dir).c_str());

        Config cfg;
        cfg.db_path           = db_dir;
        cfg.memtable_capacity = 100;
        cfg.max_levels        = 5;

        {
            LSMEngine engine(cfg);
            // 500 inserts
            for (int i = 1; i <= 500; i++) {
                engine.insert(i, "val_" + std::to_string(i));
            }
            // 300 reads
            Value out;
            int found = 0;
            for (int i = 1; i <= 300; i++) {
                if (engine.search(i, out)) found++;
            }
            // 100 updates
            for (int i = 1; i <= 100; i++) {
                engine.insert(i, "upd_" + std::to_string(i));
            }
            // 100 scans
            int total_scanned = 0;
            for (int i = 1; i <= 100; i++) {
                auto res = engine.scan(i, i + 10);
                total_scanned += res.size();
            }
            engine.flush();
            std::cout << "  Iteration " << iteration << ": 1000 ops executed. Found reads: "
                      << found << ", Scanned: " << total_scanned << "\n";
        }

        int current_fds = getOpenFileDescriptors(pid);
        std::cout << "  Iteration " << iteration << " completed. Current FDs: " << current_fds << "\n";
    }

    (void)system(("rm -rf " + db_dir).c_str());
    int final_fds = getOpenFileDescriptors(pid);
    std::cout << "Final open file descriptors: " << final_fds << "\n";

    if (final_fds <= initial_fds + 2) {
        std::cout << "[SUCCESS] Step 3 Smoke Test PASSED: No FD leaks detected!\n";
        return 0;
    } else {
        std::cout << "[FAILURE] FD leak detected: initial=" << initial_fds << ", final=" << final_fds << "\n";
        return 1;
    }
}
