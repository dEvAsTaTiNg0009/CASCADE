CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread -I./include
ASAN     = -fsanitize=address,undefined -g -O1
TSAN     = -fsanitize=thread -g -O1

SRC       = src/bloom.cpp src/cache.cpp src/csb_tree.cpp src/ahlc.cpp \
            src/sstable.cpp src/lsm.cpp

TEST_BIN        = cascade_test
BENCH_BIN       = cascade_bench
SCALE_BIN       = scale_bench
RIGOROUS_BIN    = rigorous_bench
AHLC_SWEEP_BIN  = ahlc_sweep
MT_COMPACT_BIN  = mt_compaction_bench

# ---- RocksDB (optional) ------------------------------------------------
# Requires: brew install rocksdb  (macOS)  |  sudo apt install librocksdb-dev (Linux)
ROCKSDB_AVAIL := $(shell (test -f /opt/homebrew/include/rocksdb/db.h || pkg-config --exists rocksdb 2>/dev/null) && echo 1 || echo 0)
ROCKSDB_BIN = rocksdb_bench
ifeq ($(ROCKSDB_AVAIL),1)
  ROCKSDB_FLAGS = $(shell pkg-config --cflags --libs rocksdb 2>/dev/null || echo "-I/opt/homebrew/include -L/opt/homebrew/lib -lrocksdb -lsnappy -lgflags -lzstd -llz4") -DROCKSDB_AVAILABLE=1 -Wno-c++20-extensions
else
  ROCKSDB_FLAGS =
endif

.PHONY: all test bench scale rigorous mt_compaction asan tsan clean

all: test bench scale rigorous $(AHLC_SWEEP_BIN) mt_compaction

$(TEST_BIN): $(SRC) tests/test_all.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BENCH_BIN): $(SRC) bench/ycsb_bench.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

$(SCALE_BIN): $(SRC) bench/scale_bench.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

$(RIGOROUS_BIN): $(SRC) bench/rigorous_bench.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

$(AHLC_SWEEP_BIN): $(SRC) bench/ahlc_sweep.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

$(MT_COMPACT_BIN): $(SRC) bench/mt_compaction_bench.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

ifeq ($(ROCKSDB_AVAIL),1)
$(ROCKSDB_BIN): $(SRC) bench/rocksdb_bench.cpp
	$(CXX) $(CXXFLAGS) $(ROCKSDB_FLAGS) -o $@ $^
else
$(ROCKSDB_BIN):
	@echo "  [INFO] RocksDB not found."
	@echo "  Install: brew install rocksdb  (macOS)"
	@echo "           sudo apt install librocksdb-dev  (Ubuntu)"
	@echo "  Then re-run: make rocksdb_bench"
endif

test:          $(TEST_BIN)
	./$(TEST_BIN)
bench:         $(BENCH_BIN)
scale:         $(SCALE_BIN)
rigorous:      $(RIGOROUS_BIN)
mt_compaction: $(MT_COMPACT_BIN)

asan: $(SRC) tests/test_all.cpp
	$(CXX) $(CXXFLAGS) $(ASAN) -o cascade_asan $^
	./cascade_asan

tsan: $(SRC) tests/test_all.cpp
	$(CXX) $(CXXFLAGS) $(TSAN) -o cascade_tsan $^
	./cascade_tsan

clean:
	rm -f $(TEST_BIN) $(BENCH_BIN) $(SCALE_BIN) $(RIGOROUS_BIN) \
	      $(AHLC_SWEEP_BIN) $(MT_COMPACT_BIN) $(ROCKSDB_BIN) \
	      cascade_asan cascade_tsan *.o
