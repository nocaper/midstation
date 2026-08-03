CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -D_FILE_OFFSET_BITS=64 -Wall -Wextra -Wpedantic
TARGET ?= rw_mmap_bench
LDLIBS ?= -latomic

.PHONY: all clean

all: $(TARGET)

$(TARGET): rw_mmap_bench.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)
