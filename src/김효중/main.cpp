// SOSD uint32 equality search, first matching 0-based position.
// C++17, Visual Studio 2022 x64 / GCC / Clang. Single-threaded.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using Clock = std::chrono::steady_clock;
static double nanos(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::nano>(b - a).count();
}

class MappedDataset {
#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
#else
    int fd_ = -1;
#endif
    void* mapped_ = nullptr;
    std::uint64_t bytes_ = 0;
    const std::uint32_t* keys_ = nullptr;
    std::uint64_t count_ = 0;
public:
    explicit MappedDataset(const std::string& path) {
#ifdef _WIN32
        // Convert UTF-8 to Unicode so Korean user-folder names also work.
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0);
        if (!n) throw std::runtime_error("Invalid UTF-8 dataset path");
        std::wstring wide(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, wide.data(), n);
        file_ = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open dataset (use an ASCII-only path if needed)");
        LARGE_INTEGER len{};
        if (!GetFileSizeEx(file_, &len)) throw std::runtime_error("GetFileSizeEx failed");
        bytes_ = static_cast<std::uint64_t>(len.QuadPart);
        mapping_ = CreateFileMappingA(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_) throw std::runtime_error("CreateFileMapping failed");
        mapped_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
        if (!mapped_) throw std::runtime_error("MapViewOfFile failed: use an x64 build");
#else
        fd_ = open(path.c_str(), O_RDONLY);
        if (fd_ < 0) throw std::runtime_error("Cannot open dataset");
        struct stat st{};
        if (fstat(fd_, &st) != 0) throw std::runtime_error("fstat failed");
        bytes_ = static_cast<std::uint64_t>(st.st_size);
        mapped_ = mmap(nullptr, static_cast<size_t>(bytes_), PROT_READ, MAP_PRIVATE, fd_, 0);
        if (mapped_ == MAP_FAILED) { mapped_ = nullptr; throw std::runtime_error("mmap failed"); }
#endif
        if (bytes_ < 8) throw std::runtime_error("Invalid SOSD file (header missing)");
        std::memcpy(&count_, mapped_, 8);
        if (count_ > (bytes_ - 8) / 4 || 8 + count_ * 4 != bytes_)
            throw std::runtime_error("Invalid SOSD uint32 file size/header (requires uncompressed .uint32 binary)");
        if (!count_) throw std::runtime_error("Dataset is empty");
        keys_ = reinterpret_cast<const std::uint32_t*>(static_cast<const unsigned char*>(mapped_) + 8);
        if (keys_[0] > keys_[count_ - 1]) throw std::runtime_error("Not sorted ascending");
    }
    ~MappedDataset() {
#ifdef _WIN32
        if (mapped_) UnmapViewOfFile(mapped_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
#else
        if (mapped_) munmap(mapped_, static_cast<size_t>(bytes_));
        if (fd_ >= 0) close(fd_);
#endif
    }
    MappedDataset(const MappedDataset&) = delete;
    MappedDataset& operator=(const MappedDataset&) = delete;
    std::uint64_t size() const { return count_; }
    std::uint32_t operator[](std::uint64_t i) const { return keys_[i]; }
};

#ifdef _WIN32
static void enable_job_limit() {
    // This constrains private committed memory, NOT memory-mapped file working set.
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (!job) throw std::runtime_error("CreateJobObject failed");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_JOB_MEMORY;
    info.JobMemoryLimit = static_cast<SIZE_T>(512ULL * 1024 * 1024);
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)))
        throw std::runtime_error("SetInformationJobObject failed");
    if (!AssignProcessToJobObject(job, GetCurrentProcess()))
        throw std::runtime_error("AssignProcessToJobObject failed (already in an incompatible job?)");
    // Keep handle alive for process lifetime intentionally.
}
#endif

struct Sample { std::uint32_t key; std::uint64_t position; };
class Index {
    const MappedDataset& d_;
    std::vector<Sample> samples_;
    std::uint64_t stride_;
public:
    Index(const MappedDataset& d, std::uint64_t stride, bool build) : d_(d), stride_(stride) {
        if (build) {
            samples_.reserve(static_cast<size_t>((d.size()-1)/stride + 2));
            for (std::uint64_t i=0; i<d.size(); i+=stride) samples_.push_back({d[i],i});
            if (samples_.back().position != d.size()-1) samples_.push_back({d[d.size()-1], d.size()-1});
        }
    }
    std::size_t index_bytes() const { return samples_.capacity()*sizeof(Sample); }
    std::uint64_t binary_lower_bound(std::uint32_t key, std::uint64_t lo, std::uint64_t hi) const {
        while (lo < hi) {
            auto mid = lo + (hi-lo)/2;
            if (d_[mid] < key) lo = mid+1; else hi = mid;
        }
        return lo;
    }
    std::uint64_t dense_direct(std::uint32_t key) const {
        // Valid ONLY for genuinely consecutive keys (official SOSD uniform_dense).
        // Validate the input contract when selecting this method (see main).
        if (key < d_[0]) return d_.size();
        std::uint64_t pos = static_cast<std::uint64_t>(key) - d_[0];
        return (pos < d_.size() && d_[pos] == key) ? pos : d_.size();
    }
    std::uint64_t binary(std::uint32_t key) const {
        if (key < d_[0] || key > d_[d_.size()-1]) return d_.size();
        auto pos = binary_lower_bound(key,0,d_.size());
        return (pos<d_.size() && d_[pos]==key) ? pos : d_.size();
    }
    // Find sample containing predecessor boundary; use strict < key to preserve FIRST duplicate index.
    std::pair<std::uint64_t,std::uint64_t> interval(std::uint32_t key) const {
        auto it = std::lower_bound(samples_.begin(), samples_.end(), key,
            [](const Sample& a, std::uint32_t k){return a.key < k;});
        if (it == samples_.begin()) return {0, std::min<std::uint64_t>(d_.size(),stride_+1)};
        if (it == samples_.end()) return {samples_[samples_.size()-2].position, d_.size()};
        auto prev = it-1;
        return {prev->position, std::min<std::uint64_t>(d_.size(),it->position+1)};
    }
    std::uint64_t sampled(std::uint32_t key) const {
        if (key < d_[0] || key > d_[d_.size()-1]) return d_.size();
        auto bounds = interval(key);
        auto pos = binary_lower_bound(key,bounds.first,bounds.second);
        return (pos<d_.size() && d_[pos]==key) ? pos : d_.size();
    }
    // Predict an approximate index inside a sampled block; exponential bracketing handles errors.
    std::uint64_t hybrid(std::uint32_t key) const {
        if (key < d_[0] || key > d_[d_.size()-1]) return d_.size();
        auto b = interval(key);
        const std::uint64_t lo = b.first, hi = b.second;
        if (hi <= lo) return d_.size();
        auto startKey = static_cast<std::uint64_t>(d_[lo]);
        auto endKey = static_cast<std::uint64_t>(d_[hi-1]);
        std::uint64_t guess = lo + (hi-lo)/2;
        if (endKey > startKey && static_cast<std::uint64_t>(key) >= startKey) {
            // Double avoids 64-bit multiplication overflow for future sizes.
            double fraction = double(static_cast<std::uint64_t>(key) - startKey) / double(endKey-startKey);
            guess = lo + std::min<std::uint64_t>(hi-lo-1, static_cast<std::uint64_t>(fraction*(hi-lo-1)));
        }
        std::uint64_t left=lo, right=hi;
        if (d_[guess] < key) {
            left = guess + 1;
            std::uint64_t step = 1;
            while (left < hi) {
                std::uint64_t probe = left + std::min(step-1, hi-left-1);
                if (d_[probe] >= key) { right=probe+1; break; }
                left=probe+1;
                if (step < (1ULL<<60)) step*=2;
            }
        } else {
            right = guess+1;
            std::uint64_t step = 1;
            while (right > lo) {
                std::uint64_t probe = (right-lo <= step) ? lo : right-step;
                if (d_[probe] < key) { left=probe+1; break; }
                right=probe+1;
                if (probe == lo) { left=lo; break; }
                if (step < (1ULL<<60)) step*=2;
            }
        }
        auto pos=binary_lower_bound(key,left,right);
        return (pos<d_.size() && d_[pos]==key) ? pos : d_.size();
    }
    std::uint64_t find(std::uint32_t key, const std::string& mode) const {
        if (mode=="dense") return dense_direct(key);
        if (mode=="binary") return binary(key);
        if (mode=="sampled") return sampled(key);
        return hybrid(key);
    }
};

static std::uint32_t parse_key(const std::string& s) {
    size_t used=0;
    auto v=std::stoull(s,&used,10);
    if (used!=s.size() || v>std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("Key must be uint32 (0..4294967295)");
    return static_cast<std::uint32_t>(v);
}

struct Options { std::uint64_t stride = 4096; bool verify = true; };

static std::vector<std::uint32_t> workloads(const MappedDataset& d, std::uint32_t q, bool hits) {
    std::vector<std::uint32_t> out;
    out.reserve(q);
    std::mt19937_64 rng(0x20261106);
    std::uniform_int_distribution<std::uint64_t> dist(0,d.size()-1);
    for(std::uint32_t i=0; i<q; ++i) {
        auto p=dist(rng);
        if (hits) out.push_back(d[p]);
        else {
            // Choose provably absent keys, including inside gaps, without expensive pre-computation.
            if(p+1<d.size() && static_cast<std::uint64_t>(d[p+1])>static_cast<std::uint64_t>(d[p])+1ULL)
                out.push_back(d[p]+1);
            else if(d[0]>0) out.push_back(d[0]-1);
            else if(d[d.size()-1]<UINT32_MAX) out.push_back(d[d.size()-1]+1);
            else {
                // For pathological domains use rejection by binary search (untimed).
                std::uint32_t candidate=static_cast<std::uint32_t>(rng());
                Index baseline(d,4096,false);
                for(unsigned tries=0; tries<100000 && baseline.binary(candidate)!=d.size(); ++tries) candidate=static_cast<std::uint32_t>(rng());
                if(baseline.binary(candidate)!=d.size()) throw std::runtime_error("Unable to generate absent key");
                out.push_back(candidate);
            }
        }
    }
    return out;
}

struct Result { std::string method; double build=0, hit=0, miss=0; std::size_t bytes=0; std::uint64_t checksum=0; };
static Result run(const MappedDataset& d,const Options& o,const std::string& method,
    const std::vector<std::uint32_t>& hitKeys,const std::vector<std::uint32_t>& missKeys) {
    auto t0=Clock::now();
    Index idx(d,o.stride,method!="binary" && method!="dense");
    auto t1=Clock::now();
    Result r; r.method=method; r.build=nanos(t0,t1); r.bytes=idx.index_bytes();
    auto run_batch=[&](const std::vector<std::uint32_t>& keys) {
        std::uint64_t check=0;
        auto start=Clock::now();
        for(auto key:keys) {
            auto pos=idx.find(key,method);
            check += (pos==d.size() ? 0 : (pos+1));
        }
        auto end=Clock::now();
        r.checksum += check;
        return nanos(start,end);
    };
    r.hit=run_batch(hitKeys);
    r.miss=run_batch(missKeys);
    if (o.verify) {
        for(auto key:hitKeys) if(idx.find(key,method)!=idx.binary(key)) throw std::runtime_error("VALIDATION FAILED (hit) for "+method);
        for(auto key:missKeys) if(idx.find(key,method)!=idx.binary(key)) throw std::runtime_error("VALIDATION FAILED (miss) for "+method);
    }
    return r;
}

// 데이터셋 경로는 프로그램 실행 시 사용자에게 입력받는다.
// 사용자는 자료구조를 선택하지 않는다.
// Dense: 연속 정수의 직접 위치 계산, Sparse: 샘플 인덱스 + 보간/지수 탐색.
static std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
static std::string input_line(const std::string& text) {
    std::cout << text << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) throw std::runtime_error("입력이 종료되었습니다.");
    return trim(line);
}
static bool official_dense(const MappedDataset& d) {
    // 공식 데이터셋의 연속값 규약을 확인한다. 임의의 파일에는 적용하면 안 된다.
    return d.size() == 200000000ULL && d[0] == 42U && d[d.size()-1] == 200000041U;
}
static void search_keys(const MappedDataset& d, bool dense) {
    if (dense && !official_dense(d)) {
        std::cout << "\n[오류] Dense 데이터셋이 공식 2억 개 연속 정수 형식이 아닙니다.\n"
                  << "파일 내용을 확인해 주세요. (잘못된 위치 계산 방지)\n";
        return;
    }
    const std::string method = dense ? "dense" : "hybrid";
    constexpr std::uint64_t stride = 4096;
    const auto start = Clock::now();
    Index index(d, stride, !dense);
    const auto finish = Clock::now();
    const double build_ns = nanos(start, finish);
    double sum_search_ns = 0.0;
    std::uint64_t query_count = 0;
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "\n========================================\n";
    std::cout << " 자동 선택된 검색 방식: "
              << (dense ? "연속 정수 직접 위치 계산 (O(1))" : "하이브리드 검색 (샘플 인덱스 + 보간/지수 탐색)") << "\n";
    std::cout << " 인덱스 빌드 시간: " << build_ns << " ns\n";
    std::cout << " 추가 인덱스 공간: " << index.index_bytes() << " 바이트\n";
    std::cout << "========================================\n";
    std::cout << "검색할 정수 키를 입력하세요. (0~4294967295)\n";
    std::cout << "뒤로 가려면 q를 입력하세요.\n\n";
    while (true) {
        auto line = input_line("검색할 정수 키 > ");
        if (line == "q" || line == "Q") break;
        try {
            const auto key = parse_key(line);
            const auto t0 = Clock::now();
            const auto position = index.find(key, method);
            const auto t1 = Clock::now();
            const double search_ns = nanos(t0, t1);
            sum_search_ns += search_ns;
            ++query_count;
            std::cout << "\n[검색 결과]\n";
            std::cout << "입력한 정수 키: " << key << "\n";
            if (position == d.size()) {
                std::cout << "결과: 데이터셋에 없는 값입니다.\n";
            } else {
                std::cout << "결과: 찾았습니다!\n"
                          << "데이터셋의 위치 (1부터): " << (position + 1) << "번째\n"
                          << "배열 인덱스 (0부터): " << position << "\n";
            }
            std::cout << "인덱스 빌드 시간: " << build_ns << " ns\n";
            std::cout << "이번 키 검색 시간: " << search_ns << " ns\n";
            std::cout << "빌드 + 이번 키 검색: " << (build_ns + search_ns) << " ns\n";
            std::cout << "누적 " << query_count << "회 검색 시간: " << sum_search_ns << " ns\n";
            std::cout << "빌드 + 누적 검색 시간: " << (build_ns + sum_search_ns) << " ns\n\n";
        } catch (const std::exception&) {
            std::cout << "[입력 오류] 0부터 4294967295까지의 정수를 입력하세요.\n\n";
        }
    }
}
// 선택된 방식과 다른 자료구조를 동일한 조건으로 비교해 결과를 검증하는 기능.
// 이 비교 메뉴에서도 사용자에게 자료구조를 선택하게 하지 않는다.
static void compare_all(const MappedDataset& d, bool dense) {
    if (dense && !official_dense(d)) {
        std::cout << "공식 Dense 데이터셋이 아니므로 비교할 수 없습니다.\n";
        return;
    }
    constexpr std::uint32_t queries = 100000;
    constexpr std::uint64_t stride = 4096;
    std::cout << "\n존재하는 키 " << queries << "개와 없는 키 " << queries
              << "개를 준비합니다. (쿼리 준비 시간은 측정에서 제외)\n";
    const auto hits = workloads(d,queries,true);
    const auto misses = workloads(d,queries,false);
    const Options options{stride,true};
    const std::vector<std::string> methods = dense
        ? std::vector<std::string>{"dense","binary","sampled","hybrid"}
        : std::vector<std::string>{"binary","sampled","hybrid"};
    std::vector<Result> results;
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "\n방식 | 빌드(ns) | 존재 키(ns) | 부재 키(ns) | 합계(ns) | 인덱스(byte)\n";
    for (const auto& method : methods) {
        const auto r = run(d,options,method,hits,misses);
        results.push_back(r);
        std::cout << method << " | " << r.build << " | " << r.hit << " | "
                  << r.miss << " | " << r.build+r.hit+r.miss << " | " << r.bytes << "\n";
    }
    const auto best = std::min_element(results.begin(),results.end(),
        [](const Result& a,const Result& b) {
            return a.build+a.hit+a.miss < b.build+b.hit+b.miss;
        });
    std::cout << "\n이번 측정의 최단 합계 방식: " << best->method << " ("
              << best->build+best->hit+best->miss << " ns)\n";
    std::cout << "실제 사용자 검색에 자동 적용되는 방식: "
              << (dense ? "dense" : "hybrid") << "\n";
    std::cout << "※ 측정 환경에 따라 최적 순위가 달라질 수 있습니다.\n"
              << "※ 데이터 읽기와 검증 및 쿼리 생성 시간은 합계에 포함하지 않습니다.\n\n";
}
static std::string unquote_path(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = s.substr(1, s.size()-2);
    return trim(s);
}

static std::string lowercase_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// 파일명으로 데이터셋 유형 판별. 명칭을 변경한 경우 사용자에게 확인한다.
static bool detect_dense(const std::string& path, const MappedDataset& d) {
    const auto filename = lowercase_ascii(std::filesystem::u8path(path).filename().u8string());
    if (filename.find("uniform_dense") != std::string::npos) {
        if (!official_dense(d)) throw std::runtime_error("Dense 파일의 시작/끝 값 또는 개수가 예상과 다릅니다.");
        return true;
    }
    if (filename.find("uniform_sparse") != std::string::npos) return false;
    std::cout << "파일명으로 Dense/Sparse를 판별하지 못했습니다.\n"
              << "1. Uniform Dense (공식 2억 개 데이터)\n"
              << "2. Uniform Sparse 또는 일반 정렬 uint32 데이터\n";
    while (true) {
        const auto answer = input_line("데이터셋 종류 (1 또는 2) > ");
        if (answer == "1") {
            if (!official_dense(d)) throw std::runtime_error("공식 Dense 데이터 조건이 맞지 않습니다.");
            return true;
        }
        if (answer == "2") return false;
        std::cout << "1 또는 2를 입력하세요.\n";
    }
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    try {
#ifdef _WIN32
        // Job Object는 private commit 제한이며, 매핑된 파일의 물리 메모리 상한을 뜻하지 않는다.
        try { enable_job_limit(); }
        catch (const std::exception& e) {
            std::cerr << "[주의] Job Object 512MiB 제한을 적용하지 못했습니다: " << e.what() << "\n";
        }
#endif
        std::cout << "================================================\n"
                  << " SOSD 정수 키 검색 프로그램 (경로 입력 / ns 출력)\n"
                  << "================================================\n"
                  << "자료구조는 데이터 특성에 따라 자동 적용합니다.\n"
                  << "Dense: 직접 위치 계산, Sparse: 하이브리드 탐색\n"
                  << "단위 ns = 나노초 (1ms = 1,000,000ns)\n";
        for (;;) {
            std::cout << "\n데이터셋 전체 파일 경로를 입력하세요.\n"
                      << "예: C:\\SOSD_Data\\uniform_sparse_200M_uint32\n"
                      << "경로에 공백이나 한글이 있어도 됩니다. (q: 종료)\n";
            const auto raw = input_line("데이터셋 경로 > ");
            if (raw == "q" || raw == "Q") break;
            const auto path = unquote_path(raw);
            if (path.empty()) {
                std::cout << "파일 경로를 입력해 주세요.\n";
                continue;
            }
            try {
                MappedDataset dataset(path);
                const bool dense = detect_dense(path, dataset);
                std::cout << "\n[데이터셋 연결 성공]\n"
                          << "파일 종류: " << (dense ? "Uniform Dense" : "Uniform Sparse / 정렬 uint32") << "\n"
                          << "데이터 개수: " << dataset.size() << "개\n";
                for (;;) {
                    std::cout << "\n[작업 선택]\n"
                              << " 1. 정수 키 입력 후 위치 검색\n"
                              << " 2. 자료구조별 검색 속도 자동 비교\n"
                              << " 0. 다른 데이터셋 파일 선택\n";
                    const auto action = input_line("번호 입력 > ");
                    if (action == "0") break;
                    if (action == "1") search_keys(dataset,dense);
                    else if (action == "2") compare_all(dataset,dense);
                    else std::cout << "0, 1 또는 2를 입력하세요.\n";
                }
            } catch (const std::exception& e) {
                std::cerr << "[데이터셋 오류] " << e.what() << "\n"
                          << "파일 경로, 바이너리 형식 및 64비트 실행 여부를 확인해 주세요.\n";
            }
        }
        std::cout << "프로그램을 종료합니다.\n";
    } catch (const std::exception& e) {
        std::cerr << "[오류] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
