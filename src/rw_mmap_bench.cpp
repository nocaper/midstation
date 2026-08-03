#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t kMagic = 0x5257424d;  // "RWBM"
constexpr uint32_t kWriterBit = 1u << 31;
constexpr uint32_t kYieldEvery = 1000;
constexpr size_t kChunkAlign = 4096;
constexpr size_t kHeaderBytes = 4096;
static_assert(kHeaderBytes % kChunkAlign == 0, "data buffer must be 4K aligned");

struct alignas(kChunkAlign) SharedHeader {
    uint32_t magic;
    uint32_t rwlock;
    uint32_t reader_ready;
    uint32_t error;

    uint64_t chunk_size;
    uint64_t total_size;
    uint64_t chunk_count;
    uint64_t seq;
    uint64_t ack_seq;
    uint64_t write_ns_sum;
    uint64_t read_ns_sum;
};

static_assert(sizeof(SharedHeader) <= kHeaderBytes, "shared header is too large");
static_assert(alignof(SharedHeader) == kChunkAlign, "shared header must be 4K aligned");
static_assert(std::is_standard_layout<SharedHeader>::value, "shared header layout must be stable");
static_assert(std::is_trivially_copyable<SharedHeader>::value,
              "shared header must be safe in raw shared memory");

struct Chunk {
    size_t offset;
    size_t size;
    size_t shared_offset;
};

struct Plan {
    size_t chunk_size = 0;
    std::vector<uint8_t> payload;
    std::vector<Chunk> chunks;
};

struct Config {
    std::string role;
    std::string device;
    std::string input;
    uint64_t physical_address = 0;
    size_t map_size = 0;
};

struct Mapping {
    int fd = -1;
    void* base = MAP_FAILED;
    size_t size = 0;
    uint8_t* region = nullptr;
};

[[noreturn]] void die(const std::string& msg) {
    throw std::runtime_error(msg);
}

std::string trim(const std::string& s) {
    size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) {
        ++first;
    }

    size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) {
        --last;
    }

    return s.substr(first, last - first);
}

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

uint64_t parse_u64(const std::string& raw, const std::string& name) {
    std::string s = trim(raw);
    if (s.empty()) {
        die("empty number: " + name);
    }
    if (s.front() == '-') {
        die("negative number is not allowed for " + name);
    }

    uint64_t mul = 1;
    char suffix = static_cast<char>(std::tolower(static_cast<unsigned char>(s.back())));
    if (suffix == 'k' || suffix == 'm' || suffix == 'g') {
        s.pop_back();
        mul = suffix == 'k' ? 1024ull : suffix == 'm' ? 1024ull * 1024ull
                                                       : 1024ull * 1024ull * 1024ull;
    }

    size_t pos = 0;
    uint64_t value = 0;
    try {
        value = std::stoull(s, &pos, 0);
    } catch (const std::exception&) {
        die("invalid number for " + name + ": " + raw);
    }
    if (pos != s.size()) {
        die("invalid number for " + name + ": " + raw);
    }
    if (value > std::numeric_limits<uint64_t>::max() / mul) {
        die("number overflow for " + name);
    }
    return value * mul;
}

size_t align_up(size_t value, size_t align) {
    if (align == 0 || (align & (align - 1)) != 0) {
        die("alignment must be a power of two");
    }
    if (value > std::numeric_limits<size_t>::max() - (align - 1)) {
        die("alignment overflow");
    }
    return (value + align - 1) & ~(align - 1);
}

Config load_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        die("open config failed: " + path);
    }

    std::unordered_map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            die("bad config line: " + line);
        }
        kv[lower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
    }

    Config cfg;
    if (kv.count("role")) {
        cfg.role = lower(kv["role"]);
    }
    if (kv.count("device")) {
        cfg.device = kv["device"];
    }
    if (kv.count("input")) {
        cfg.input = kv["input"];
    }
    if (kv.count("physical_address")) {
        cfg.physical_address = parse_u64(kv["physical_address"], "physical_address");
    }
    if (kv.count("map_size")) {
        uint64_t n = parse_u64(kv["map_size"], "map_size");
        if (n > std::numeric_limits<size_t>::max()) {
            die("map_size is too large");
        }
        cfg.map_size = static_cast<size_t>(n);
    }

    if (cfg.role != "writer" && cfg.role != "reader") {
        die("role must be writer or reader");
    }
    if (cfg.device.empty()) {
        die("device is required");
    }
    if (!kv.count("physical_address")) {
        die("physical_address is required");
    }
    if (cfg.map_size == 0) {
        die("map_size is required");
    }
    if (cfg.input.empty()) {
        die("input is required");
    }

    return cfg;
}

Plan load_input(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        die("open input failed: " + path);
    }

    std::string first_line;
    if (!std::getline(in, first_line)) {
        die("input file is empty");
    }

    uint64_t chunk = parse_u64(first_line, "input first line");
    if (chunk == 0 || chunk > std::numeric_limits<size_t>::max()) {
        die("bad input granularity");
    }

    Plan plan;
    plan.chunk_size = static_cast<size_t>(chunk);
    plan.payload.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (plan.payload.empty()) {
        die("input payload is empty");
    }

    size_t shared_offset = 0;
    for (size_t off = 0; off < plan.payload.size();) {
        size_t left = plan.payload.size() - off;
        size_t size = left < plan.chunk_size ? left : plan.chunk_size;
        plan.chunks.push_back({off, size, shared_offset});

        size_t slot_size = align_up(size, kChunkAlign);
        if (shared_offset > std::numeric_limits<size_t>::max() - slot_size) {
            die("mapped chunk layout overflows size_t");
        }
        shared_offset += slot_size;

        if (off > std::numeric_limits<size_t>::max() - size) {
            die("input chunk offset overflows size_t");
        }
        off += size;
    }
    return plan;
}

uint64_t now_ns() {
    timespec ts{};
#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

uint32_t load32(const uint32_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

uint64_t load64(const uint64_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

void store32(uint32_t* p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

void store64(uint64_t* p, uint64_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

void pause_spin(uint64_t& spins) {
    if (++spins % kYieldEvery == 0) {
        sched_yield();
    }
}

void keep_memcpy_visible(const void* ptr, size_t size) {
    asm volatile("" : : "r"(ptr), "r"(size) : "memory");
}

void wait32(const uint32_t* p, uint32_t want, SharedHeader* h) {
    uint64_t spins = 0;
    while (load32(p) != want) {
        if (load32(&h->error) != 0) {
            die("peer error");
        }
        pause_spin(spins);
    }
}

void wait64(const uint64_t* p, uint64_t want, SharedHeader* h) {
    uint64_t spins = 0;
    while (load64(p) != want) {
        if (load32(&h->error) != 0) {
            die("peer error");
        }
        pause_spin(spins);
    }
}

void wrlock(SharedHeader* h) {
    uint64_t spins = 0;
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&h->rwlock, &expected, kWriterBit, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return;
        }
        pause_spin(spins);
    }
}

void wrunlock(SharedHeader* h) {
    store32(&h->rwlock, 0);
}

void rdlock(SharedHeader* h) {
    uint64_t spins = 0;
    for (;;) {
        uint32_t cur = load32(&h->rwlock);
        if ((cur & kWriterBit) != 0) {
            pause_spin(spins);
            continue;
        }
        uint32_t next = cur + 1;
        if (__atomic_compare_exchange_n(&h->rwlock, &cur, next, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            return;
        }
        pause_spin(spins);
    }
}

void rdunlock(SharedHeader* h) {
    __atomic_fetch_sub(&h->rwlock, 1u, __ATOMIC_RELEASE);
}

Mapping map_device(const Config& cfg) {
    long ps_long = sysconf(_SC_PAGESIZE);
    if (ps_long <= 0) {
        die("sysconf page size failed");
    }

    uint64_t page_size = static_cast<uint64_t>(ps_long);
    uint64_t page_mask = page_size - 1;
    if ((page_size & page_mask) != 0) {
        die("page size is not power of two");
    }
    if (cfg.physical_address % kChunkAlign != 0) {
        die("physical_address must be 4K aligned");
    }

    uint64_t aligned = cfg.physical_address & ~page_mask;
    uint64_t delta = cfg.physical_address - aligned;
    if (aligned > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        die("physical_address exceeds mmap off_t range");
    }
    if (delta > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        die("mmap page delta exceeds size_t range");
    }

    size_t delta_size = static_cast<size_t>(delta);
    if (cfg.map_size > std::numeric_limits<size_t>::max() - delta_size) {
        die("mmap size overflows size_t");
    }
    size_t mmap_size = cfg.map_size + delta_size;

    int fd = open(cfg.device.c_str(), O_RDWR | O_SYNC);
    if (fd < 0) {
        die("open device failed: " + std::string(std::strerror(errno)));
    }

    void* base = mmap(nullptr, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                      static_cast<off_t>(aligned));
    if (base == MAP_FAILED) {
        int e = errno;
        close(fd);
        die("mmap failed: " + std::string(std::strerror(e)));
    }

    return {fd, base, mmap_size, static_cast<uint8_t*>(base) + delta};
}

void unmap_device(Mapping& m) {
    if (m.base != MAP_FAILED) {
        munmap(m.base, m.size);
        m.base = MAP_FAILED;
    }
    if (m.fd >= 0) {
        close(m.fd);
        m.fd = -1;
    }
}

void check_size(const Config& cfg, const Plan& plan) {
    if (cfg.map_size <= kHeaderBytes) {
        die("map_size must be greater than 4096");
    }

    size_t data_capacity = cfg.map_size - kHeaderBytes;
    const Chunk& last = plan.chunks.back();
    size_t required = last.shared_offset + last.size;
    if (data_capacity < required) {
        die("map_size is too small for all 4K-aligned chunk slots");
    }
}

bool is_4k_aligned(const void* p) {
    return reinterpret_cast<uintptr_t>(p) % kChunkAlign == 0;
}

void check_mapping_address(const uint8_t* region) {
    if (!is_4k_aligned(region)) {
        die("mapped shared region is not 4K aligned");
    }
    if (!is_4k_aligned(region + kHeaderBytes)) {
        die("mapped chunk data address is not 4K aligned");
    }
}

void print_result(const char* role, uint64_t ns, size_t bytes, size_t chunks) {
    double ms = static_cast<double>(ns) / 1000000.0;
    std::cout << "role=" << role << "\n";
    std::cout << "chunks=" << chunks << "\n";
    std::cout << "bytes=" << bytes << "\n";
    std::cout << "chunk_memcpy_latency_sum_ns=" << ns << "\n";
    std::cout << "chunk_memcpy_latency_sum_ms=" << ms << "\n";
}

void run_writer(SharedHeader* h, uint8_t* data, const Plan& plan) {
    std::memset(h, 0, sizeof(*h));
    store64(&h->chunk_size, plan.chunk_size);
    store64(&h->total_size, plan.payload.size());
    store64(&h->chunk_count, plan.chunks.size());
    store32(&h->magic, kMagic);

    wait32(&h->reader_ready, 1, h);

    uint64_t write_ns_sum = 0;

    for (size_t i = 0; i < plan.chunks.size(); ++i) {
        wait64(&h->ack_seq, i, h);

        const Chunk& c = plan.chunks[i];
        uint8_t* chunk_data = data + c.shared_offset;
        wrlock(h);
        uint64_t start = now_ns();
        std::memcpy(chunk_data, plan.payload.data() + c.offset, c.size);
        keep_memcpy_visible(chunk_data, c.size);
        uint64_t end = now_ns();
        wrunlock(h);

        write_ns_sum += end - start;
        store64(&h->write_ns_sum, write_ns_sum);
        store64(&h->seq, i + 1);
    }

    wait64(&h->ack_seq, plan.chunks.size(), h);
    print_result("writer", write_ns_sum, plan.payload.size(), plan.chunks.size());
}

void run_reader(SharedHeader* h, uint8_t* data, const Plan& plan) {
    uint64_t spins = 0;
    while (load32(&h->magic) != kMagic) {
        pause_spin(spins);
    }

    if (load64(&h->chunk_size) != plan.chunk_size ||
        load64(&h->total_size) != plan.payload.size() ||
        load64(&h->chunk_count) != plan.chunks.size()) {
        die("reader input does not match writer input");
    }

    std::vector<uint8_t> scratch(plan.chunk_size);
    store32(&h->reader_ready, 1);

    uint64_t read_ns_sum = 0;
    for (size_t i = 0; i < plan.chunks.size(); ++i) {
        uint64_t want = i + 1;
        spins = 0;
        while (load64(&h->seq) < want) {
            if (load32(&h->error) != 0) {
                die("peer error");
            }
            pause_spin(spins);
        }

        rdlock(h);
        const Chunk& c = plan.chunks[i];
        uint8_t* chunk_data = data + c.shared_offset;
        uint64_t start = now_ns();
        std::memcpy(scratch.data(), chunk_data, c.size);
        keep_memcpy_visible(scratch.data(), c.size);
        uint64_t end = now_ns();
        rdunlock(h);

        read_ns_sum += end - start;
        store64(&h->read_ns_sum, read_ns_sum);
        store64(&h->ack_seq, want);
    }

    print_result("reader", read_ns_sum, plan.payload.size(), plan.chunks.size());
    std::cout << "write_plus_read_memcpy_latency_sum_ns="
              << (load64(&h->write_ns_sum) + read_ns_sum) << "\n";
}

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " <config.conf>\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        usage(argv[0]);
        return 2;
    }

    Mapping mapping;
    try {
        Config cfg = load_config(argv[1]);
        Plan plan = load_input(cfg.input);
        check_size(cfg, plan);
        mapping = map_device(cfg);

        check_mapping_address(mapping.region);
        auto* h = reinterpret_cast<SharedHeader*>(mapping.region);
        uint8_t* data = mapping.region + kHeaderBytes;

        if (cfg.role == "writer") {
            run_writer(h, data, plan);
        } else {
            run_reader(h, data, plan);
        }

        unmap_device(mapping);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        if (mapping.region != nullptr && is_4k_aligned(mapping.region)) {
            auto* h = reinterpret_cast<SharedHeader*>(mapping.region);
            store32(&h->error, 1);
        }
        unmap_device(mapping);
        return 1;
    }
}
