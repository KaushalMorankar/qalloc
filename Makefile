CXX      ?= g++
CXXFLAGS ?= -O2 -g -std=c++17 -Wall -Wextra -Iinclude
LIBFLAGS  = -fPIC -fno-exceptions -fno-rtti -ftls-model=initial-exec
BUILD     = build

all: $(BUILD)/libqalloc.so $(BUILD)/test_qalloc $(BUILD)/bench

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/libqalloc.so: src/qalloc.cpp src/preload.cpp include/qalloc.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(LIBFLAGS) -fno-builtin-malloc -fno-builtin-free -fno-builtin-calloc \
	    -fno-builtin-realloc -shared -o $@ src/qalloc.cpp src/preload.cpp -pthread

$(BUILD)/test_qalloc: tests/test_qalloc.cpp src/qalloc.cpp include/qalloc.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tests/test_qalloc.cpp src/qalloc.cpp -pthread

$(BUILD)/test_qalloc_tsan: tests/test_qalloc.cpp src/qalloc.cpp include/qalloc.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=thread -o $@ tests/test_qalloc.cpp src/qalloc.cpp -pthread

$(BUILD)/bench: bench/bench.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ bench/bench.cpp -pthread

test: $(BUILD)/test_qalloc
	@for n in 1 2 7 64; do echo "== QA_ARENAS=$$n"; QA_ARENAS=$$n ./$(BUILD)/test_qalloc || exit 1; done

bench: $(BUILD)/libqalloc.so $(BUILD)/bench
	@echo "=== glibc malloc ==="; ./$(BUILD)/bench
	@echo "=== qalloc ==="; LD_PRELOAD=./$(BUILD)/libqalloc.so ./$(BUILD)/bench

clean:
	rm -rf $(BUILD)

.PHONY: all test bench clean
