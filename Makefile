CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread -I./include
ASAN     = -fsanitize=address,undefined -g -O1
TSAN     = -fsanitize=thread -g -O1

SRC       = src/bloom.cpp src/cache.cpp src/csb_tree.cpp src/ahlc.cpp \
            src/sstable.cpp src/lsm.cpp

TEST_BIN  = cascade_test
BENCH_BIN = cascade_bench
SCALE_BIN = scale_bench

.PHONY: all test bench scale asan tsan clean

all: test bench scale

$(TEST_BIN): $(SRC) tests/test_all.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BENCH_BIN): $(SRC) bench/ycsb_bench.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

$(SCALE_BIN): $(SRC) bench/scale_bench.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

test:  $(TEST_BIN)
	./$(TEST_BIN)
bench: $(BENCH_BIN)
scale: $(SCALE_BIN)

asan: $(SRC) tests/test_all.cpp
	$(CXX) $(CXXFLAGS) $(ASAN) -o cascade_asan $^
	./cascade_asan

tsan: $(SRC) tests/test_all.cpp
	$(CXX) $(CXXFLAGS) $(TSAN) -o cascade_tsan $^
	./cascade_tsan

clean:
	rm -f $(TEST_BIN) $(BENCH_BIN) $(SCALE_BIN) cascade_asan cascade_tsan *.o
