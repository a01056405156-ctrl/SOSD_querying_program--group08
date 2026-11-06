
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

using Clock = std::chrono::steady_clock;

constexpr int64_t NOT_FOUND = -1;
constexpr uint32_t RADIX_BITS = 16;
constexpr uint32_t RADIX_SIZE = 1u << RADIX_BITS;
constexpr size_t QUERY_COUNT = 2'000'000;

// Apply a 512 MiB private committed memory limit.
bool EnforceMemoryLimit512MB() {
    static HANDLE job = nullptr;

    job = CreateJobObjectA(nullptr, nullptr);
    if (!job) return false;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_PROCESS_MEMORY;

    info.ProcessMemoryLimit = 512ULL * 1024 * 1024;

    if (!SetInformationJobObject(
            job,
            JobObjectExtendedLimitInformation,
            &info,
            sizeof(info))) {
        return false;
    }

    return AssignProcessToJobObject(
        job, GetCurrentProcess()) != 0;
}

void PrintMemoryUsage() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};

    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
            sizeof(pmc))) {

        const double mib = 1024.0 * 1024.0;

        std::printf(
            "Memory: private %.2f MiB, "
            "working set %.2f MiB\n",
            pmc.PrivateUsage / mib,
            pmc.WorkingSetSize / mib);
    }
}

// Windows memory-mapped SOSD uint32 file.
class SOSDFile {
private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    const void* view_ = nullptr;
    const uint32_t* keys_ = nullptr;
    uint64_t count_ = 0;

public:
    SOSDFile() = default;
    SOSDFile(const SOSDFile&) = delete;
    SOSDFile& operator=(const SOSDFile&) = delete;

    ~SOSDFile() {
        Close();
    }

    void Close() {
        keys_ = nullptr;
        count_ = 0;

        if (view_) {
            UnmapViewOfFile(view_);
            view_ = nullptr;
        }

        if (mapping_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }

        if (file_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
    }

    bool Open(const char* path) {
        Close();

        file_ = CreateFileA(
            path,
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (file_ == INVALID_HANDLE_VALUE)
            return false;

        LARGE_INTEGER file_size{};

        if (!GetFileSizeEx(file_, &file_size))
            return false;

        if (file_size.QuadPart < 8)
            return false;

        mapping_ = CreateFileMappingA(
            file_,
            nullptr,
            PAGE_READONLY,
            0,
            0,
            nullptr);

        if (!mapping_)
            return false;

        view_ = MapViewOfFile(
            mapping_,
            FILE_MAP_READ,
            0,
            0,
            0);

        if (!view_)
            return false;

        const auto* bytes =
            static_cast<const unsigned char*>(view_);

        uint64_t n = 0;

        for (int i = 0; i < 8; ++i) {
            n |= uint64_t(bytes[i]) << (i * 8);
        }

        if (n == 0 ||
            n > (std::numeric_limits<uint64_t>::max() - 8) / 4)
            return false;

        if (uint64_t(file_size.QuadPart) != 8 + n * 4)
            return false;

        if (n > std::numeric_limits<uint32_t>::max())
            return false;

        count_ = n;
        keys_ = reinterpret_cast<const uint32_t*>(bytes + 8);

        return true;
    }

    const uint32_t* data() const {
        return keys_;
    }

    uint32_t size() const {
        return static_cast<uint32_t>(count_);
    }
};

// Radix index with lower-bound binary search.
class FastSOSDIndex {
private:
    const uint32_t* data_ = nullptr;
    uint32_t count_ = 0;
    uint32_t minimum_ = 0;
    uint32_t maximum_ = 0;
    uint32_t shift_ = 0;

    std::vector<uint32_t> offsets_;

    uint32_t Bucket(uint32_t key) const {
        uint64_t difference =
            uint64_t(key) - uint64_t(minimum_);

        uint64_t bucket = difference >> shift_;

        if (bucket >= RADIX_SIZE)
            bucket = RADIX_SIZE - 1;

        return static_cast<uint32_t>(bucket);
    }

public:
    void BuildFromArray(
        const uint32_t* sorted,
        uint32_t count) {

        if (!sorted || count == 0)
            throw std::runtime_error("Empty dataset");

        data_ = sorted;
        count_ = count;
        minimum_ = data_[0];
        maximum_ = data_[count_ - 1];

        if (maximum_ < minimum_)
            throw std::runtime_error("Invalid key range");

        uint64_t range =
            uint64_t(maximum_) - minimum_;

        shift_ = 0;

        while ((range >> shift_) >= RADIX_SIZE)
            ++shift_;

        offsets_.assign(RADIX_SIZE + 1, count_);

        uint32_t position = 0;

        for (uint32_t bucket = 0;
             bucket < RADIX_SIZE;
             ++bucket) {

            while (position < count_ &&
                   Bucket(data_[position]) < bucket) {
                ++position;
            }

            offsets_[bucket] = position;
        }

        offsets_[RADIX_SIZE] = count_;
    }

    int64_t Search(uint32_t key) const {
        if (!data_ || count_ == 0)
            return NOT_FOUND;

        if (key < minimum_ || key > maximum_)
            return NOT_FOUND;

        uint32_t bucket = Bucket(key);

        uint32_t lo = offsets_[bucket];
        uint32_t hi = offsets_[bucket + 1];

        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;

            if (data_[mid] < key)
                lo = mid + 1;
            else
                hi = mid;
        }

        if (lo < count_ && data_[lo] == key)
            return static_cast<int64_t>(lo);

        return NOT_FOUND;
    }

    uint32_t size() const {
        return count_;
    }

    uint32_t minimum() const {
        return minimum_;
    }

    uint32_t maximum() const {
        return maximum_;
    }
};

void RunBenchmark(
    const FastSOSDIndex& index,
    const uint32_t* data,
    uint32_t count,
    double build_ms,
    bool real_dataset) {

    std::vector<uint32_t> queries(QUERY_COUNT);

    std::mt19937_64 rng(2022202090);

    std::uniform_int_distribution<uint32_t> pick(
        0, count - 1);

    // Choose a guaranteed absent value outside
    // the range of the sorted dataset.
    bool has_miss_key = false;
    uint32_t miss_key = 0;

    if (index.minimum() > 0) {
        miss_key = index.minimum() - 1;
        has_miss_key = true;
    } else if (index.maximum() <
               std::numeric_limits<uint32_t>::max()) {
        miss_key = index.maximum() + 1;
        has_miss_key = true;
    }

    for (size_t i = 0; i < QUERY_COUNT; ++i) {
        if (has_miss_key && i % 2 == 1) {
            queries[i] = miss_key;
        } else {
            queries[i] = data[pick(rng)];
        }
    }

    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t errors = 0;
    int64_t checksum = 0;

    auto start = Clock::now();

    for (size_t i = 0; i < QUERY_COUNT; ++i) {
        uint32_t key = queries[i];
        int64_t found = index.Search(key);

        bool should_hit =
            !has_miss_key || (i % 2 == 0);

        if (found == NOT_FOUND) {
            ++misses;

            if (should_hit)
                ++errors;
        } else {
            ++hits;
            checksum ^= found;

            if (!should_hit ||
                found < 0 ||
                static_cast<uint64_t>(found) >= count ||
                data[found] != key) {
                ++errors;
            }
        }
    }

    double search_ms =
        std::chrono::duration<double, std::milli>(
            Clock::now() - start).count();

    std::puts(
        real_dataset
            ? "\nREAL SOSD BENCHMARK"
            : "\nSYNTHETIC BENCHMARK");

    std::printf(
        "Queries: %zu, hits: %llu, "
        "misses: %llu\n",
        QUERY_COUNT,
        static_cast<unsigned long long>(hits),
        static_cast<unsigned long long>(misses));

    std::printf(
        "Search: %.3f ms, "
        "average: %.2f ns/query, "
        "total: %.3f ms\n",
        search_ms,
        search_ms * 1e6 / QUERY_COUNT,
        build_ms + search_ms);

    std::printf(
        "Checksum: %lld\n",
        static_cast<long long>(checksum));

    std::printf(
        "Validation errors: %llu\n",
        static_cast<unsigned long long>(errors));

    if (!has_miss_key) {
        std::puts(
            "Note: no guaranteed out-of-range "
            "miss key; all queries were hits.");
    }

    if (errors == 0)
        std::puts("Benchmark validation: PASS");
    else
        std::puts("Benchmark validation: FAIL");
}

int main(int argc, char* argv[]) {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    if (EnforceMemoryLimit512MB()) {
        std::puts(
            "512 MiB process memory limit enabled.");
    } else {
        std::puts(
            "WARNING: 512 MiB memory limit "
            "could not be enabled.");
    }

    const char* dataset = nullptr;
    uint32_t test_count = 1'000'000;

    if (argc > 1) {
        if (std::string(argv[1]) == "--test") {
            if (argc > 2) {
                try {
                    unsigned long long n =
                        std::stoull(argv[2]);

                    if (n == 0 || n > 100'000'000)
                        throw std::out_of_range("count");

                    test_count = static_cast<uint32_t>(n);
                } catch (...) {
                    std::cerr
                        << "Usage: main.exe "
                        << "--test [1..100000000]\n";
                    return 1;
                }
            }
        } else {
            dataset = argv[1];
        }
    }

    SOSDFile file;
    std::vector<uint32_t> synthetic;
    FastSOSDIndex index;

    const uint32_t* data = nullptr;
    uint32_t count = 0;

    try {
        auto start = Clock::now();

        if (dataset) {
            std::printf(
                "Loading SOSD uint32 file: %s\n",
                dataset);

            if (!file.Open(dataset)) {
                std::cerr
                    << "Cannot open SOSD uint32 file.\n";
                return 1;
            }

            data = file.data();
            count = file.size();
        } else {
            std::printf(
                "Generating %u synthetic "
                "sorted uint32 keys...\n",
                test_count);

            synthetic.resize(test_count);

            uint64_t current = 100;

            for (uint32_t i = 0;
                 i < test_count;
                 ++i) {

                current += (i % 3 == 0) ? 5 : 2;

                synthetic[i] =
                    static_cast<uint32_t>(current);
            }

            data = synthetic.data();
            count = test_count;

            start = Clock::now();
        }

        index.BuildFromArray(data, count);

        double build_ms =
            std::chrono::duration<double, std::milli>(
                Clock::now() - start).count();

        std::printf(
            "Index built: %u keys, %.3f ms\n",
            index.size(),
            build_ms);

        PrintMemoryUsage();

        RunBenchmark(
            index,
            data,
            count,
            build_ms,
            dataset != nullptr);

        std::puts(
            "\nEnter a uint32 key to search, "
            "or q to quit:");

        for (std::string input;
             std::cout << "Query Key > " &&
             std::cin >> input;) {

            if (input == "q" ||
                input == "Q" ||
                input == "-1")
                break;

            try {
                size_t consumed = 0;

                unsigned long long value =
                    std::stoull(input, &consumed);

                if (consumed != input.size() ||
                    value >
                    std::numeric_limits<uint32_t>::max()) {
                    throw std::out_of_range("key");
                }

                uint32_t key =
                    static_cast<uint32_t>(value);

                auto search_start = Clock::now();

                int64_t position = index.Search(key);

                double ns =
                    std::chrono::duration<double,
                                          std::nano>(
                        Clock::now() - search_start
                    ).count();

                if (position == NOT_FOUND) {
                    std::printf(
                        "Not found (%.1f ns)\n", ns);
                } else {
                    std::printf(
                        "Found at index %lld "
                        "(%.1f ns)\n",
                        static_cast<long long>(position),
                        ns);
                }
            } catch (...) {
                std::puts(
                    "Please enter a valid uint32 key.");
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
