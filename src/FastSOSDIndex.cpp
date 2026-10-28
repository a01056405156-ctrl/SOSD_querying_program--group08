#define NOMINMAX // Windows.h의 min/max 매크로 충돌 방지
#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <random>
#include <intrin.h>

// 검색 실패 시 반환할 상수
constexpr int64_t NOT_FOUND = -1;

// ============================================================================
// 1. Windows Job Object를 이용한 512MB 메모리 제한 설정 함수
// ============================================================================
bool EnforceMemoryLimit512MB() {
    HANDLE hJob = CreateJobObjectA(NULL, NULL);
    if (!hJob) return false;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = { 0 };
    // 프로세스의 Private Commit 메모리 한도를 정확히 512MB로 제한
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    jeli.ProcessMemoryLimit = 512ULL * 1024ULL * 1024ULL; // 512 MB

    if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
        CloseHandle(hJob);
        return false;
    }

    // 현재 실행 중인 프로세스를 Job Object에 할당
    if (!AssignProcessToJobObject(hJob, GetCurrentProcess())) {
        // 주의: Visual Studio 디버거 등 상위 프로세스가 이미 Job에 묶여 있는 구형 환경에서는
        // 중첩 Job이 실패할 수 있으나 Windows 10/11에서는 정상 동작합니다.
        CloseHandle(hJob);
        return false;
    }

    // 프로세스 종료 시까지 유지를 위해 핸들을 닫지 않거나 정적으로 유지
    return true;
}

// 현재 프로세스의 메모리 사용량(Private Commit) 출력 유틸리티
void PrintMemoryUsage() {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        double private_mb = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
        double working_mb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        std::printf("[메모리 상태] Private Commit: %.2f MB / 512.00 MB (Working Set: %.2f MB)\n",
            private_mb, working_mb);
    }
}

// ============================================================================
// 2. 3계층 초고속 SOSD 인메모리 인덱스 (Radix + Interpolation + Branchless)
// ============================================================================
template <typename KeyType = uint64_t>
class FastSOSDIndex {
private:
    const KeyType* data_ = nullptr;
    uint32_t num_keys_ = 0;
    KeyType min_key_ = 0;
    KeyType max_key_ = 0;

    // 22비트 Radix 테이블 = 4,194,304 엔트리 = 정확히 16MB 소모
    static constexpr uint32_t RADIX_BITS = 22;
    static constexpr uint32_t RADIX_SIZE = 1U << RADIX_BITS;
    uint32_t shift_bits_ = 0;
    std::vector<uint32_t> radix_table_;

    HANDLE hFile_ = INVALID_HANDLE_VALUE;
    HANDLE hMap_ = NULL;
    const void* mapped_view_ = nullptr;

public:
    FastSOSDIndex() = default;

    ~FastSOSDIndex() {
        if (mapped_view_) UnmapViewOfFile(mapped_view_);
        if (hMap_) CloseHandle(hMap_);
        if (hFile_ != INVALID_HANDLE_VALUE) CloseHandle(hFile_);
    }

    // SOSD 표준 바이너리 파일 매핑 및 인덱스 빌드
    bool LoadAndBuildFromFile(const char* filepath) {
        hFile_ = CreateFileA(filepath, GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
        if (hFile_ == INVALID_HANDLE_VALUE) return false;

        hMap_ = CreateFileMappingA(hFile_, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!hMap_) return false;

        mapped_view_ = MapViewOfFile(hMap_, FILE_MAP_READ, 0, 0, 0);
        if (!mapped_view_) return false;

        // SOSD 바이너리 파일 규격: 첫 8바이트 = 원소 개수(uint64_t), 그 뒤로 원소 배열
        uint64_t count = *reinterpret_cast<const uint64_t*>(mapped_view_);
        const KeyType* raw_keys = reinterpret_cast<const KeyType*>(
            static_cast<const char*>(mapped_view_) + sizeof(uint64_t)
            );

        BuildFromArray(raw_keys, static_cast<uint32_t>(count));
        return true;
    }

    // 메모리에 올라온 정렬 배열로부터 인덱스 빌드
    void BuildFromArray(const KeyType* sorted_keys, uint32_t count) {
        data_ = sorted_keys;
        num_keys_ = count;
        if (count == 0) return;

        min_key_ = data_[0];
        max_key_ = data_[count - 1];

        uint64_t range = static_cast<uint64_t>(max_key_ - min_key_);
        if (range == 0) {
            shift_bits_ = 0;
            radix_table_.assign(RADIX_SIZE + 1, 0);
            radix_table_[0] = 0;
            std::fill(radix_table_.begin() + 1, radix_table_.end(), count);
            return;
        }

        unsigned long leading_zeros = static_cast<unsigned long>(__lzcnt64(range));
        uint32_t active_bits = 64 - leading_zeros;
        shift_bits_ = (active_bits > RADIX_BITS) ? (active_bits - RADIX_BITS) : 0;

        radix_table_.resize(RADIX_SIZE + 1);

        uint32_t current_idx = 0;
        radix_table_[0] = 0;

        for (uint32_t prefix = 1; prefix < RADIX_SIZE; ++prefix) {
            uint64_t boundary_offset = static_cast<uint64_t>(prefix) << shift_bits_;
            if (boundary_offset > range) {
                std::fill(radix_table_.begin() + prefix, radix_table_.end(), count);
                return;
            }
            KeyType target_boundary = min_key_ + static_cast<KeyType>(boundary_offset);

            uint32_t step = 1;
            while (current_idx + step < count && data_[current_idx + step] < target_boundary) {
                step <<= 1;
            }
            uint32_t hi = std::min(count, current_idx + step);
            current_idx = BranchlessLowerBound(current_idx, hi, target_boundary);
            radix_table_[prefix] = current_idx;
        }
        radix_table_[RADIX_SIZE] = count;
    }

    // 키 검색 함수 (존재하면 인덱스 위치 0 ~ N-1 반환, 없으면 -1 반환)
    __forceinline int64_t Search(KeyType key) const {
        if (key < min_key_ || key > max_key_) [[unlikely]] {
            return NOT_FOUND;
        }

        uint32_t prefix = static_cast<uint32_t>((static_cast<uint64_t>(key - min_key_)) >> shift_bits_);
        uint32_t lo = radix_table_[prefix];
        uint32_t hi = radix_table_[prefix + 1];

        if (lo == hi) {
            return NOT_FOUND;
        }

        if (hi - lo > 64) [[unlikely]] {
            KeyType k_lo = data_[lo];
            KeyType k_hi = data_[hi - 1];
            if (key < k_lo || key > k_hi) return NOT_FOUND;
            if (k_hi > k_lo) {
                uint32_t pred = lo + static_cast<uint32_t>(
                    (static_cast<double>(key - k_lo) / static_cast<double>(k_hi - k_lo)) * (hi - 1 - lo)
                    );
                if (data_[pred] < key) {
                    uint32_t step = 1;
                    while (pred + step < hi && data_[pred + step] < key) step <<= 1;
                    lo = pred + (step >> 1);
                    hi = std::min(hi, pred + step);
                }
                else {
                    uint32_t step = 1;
                    while (pred >= lo + step && data_[pred - step] >= key) step <<= 1;
                    hi = pred - (step >> 1) + 1;
                    lo = (pred >= lo + step) ? (pred - step) : lo;
                }
            }
        }

        uint32_t pos = BranchlessLowerBound(lo, hi, key);
        if (pos < num_keys_ && data_[pos] == key) {
            return static_cast<int64_t>(pos);
        }
        return NOT_FOUND;
    }

    uint32_t size() const { return num_keys_; }

private:
    __forceinline uint32_t BranchlessLowerBound(uint32_t lo, uint32_t hi, KeyType target) const {
        const KeyType* base = data_ + lo;
        uint32_t len = hi - lo;

        while (len > 1) {
            uint32_t half = len >> 1;
            _mm_prefetch(reinterpret_cast<const char*>(base + (half >> 1)), _MM_HINT_T0);
            _mm_prefetch(reinterpret_cast<const char*>(base + half + (half >> 1)), _MM_HINT_T0);

            base = (base[half - 1] < target) ? (base + half) : base;
            len -= half;
        }
        if (len == 1 && (*base < target)) {
            base++;
        }
        return static_cast<uint32_t>(base - data_);
    }
};

// ============================================================================
// 3. Main 함수: 메모리 제한 적용 -> 빌드 측정 -> 쿼리 측정 -> 대화형 검색
// ============================================================================
int main(int argc, char* argv[]) {
    // 입출력 속도 향상
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    // 1. 512MB 메모리 제약 설정 (Windows Job Object)
    if (EnforceMemoryLimit512MB()) {
        std::printf("[시스템] Windows Job Object 512MB 메모리 제한이 활성화되었습니다.\n");
    }
    else {
        std::printf("[경고] Job Object 메모리 제한 설정에 실패했거나 이미 상위 Job에 속해 있습니다.\n");
    }

    FastSOSDIndex<uint64_t> index;
    std::vector<uint64_t> synthetic_data; // 파일이 없을 때 사용할 인메모리 테스트 배열

    // 명령줄 인자로 SOSD 데이터셋 파일 경로가 주어진 경우 (예: program.exe books_200M_uint64)
    const char* dataset_path = (argc > 1) ? argv[1] : nullptr;

    double build_time_ms = 0.0;

    if (dataset_path != nullptr) {
        std::printf("[데이터 로드] SOSD 파일 매핑 및 인덱스 빌드 시작: %s\n", dataset_path);
        auto start_build = std::chrono::high_resolution_clock::now();

        if (!index.LoadAndBuildFromFile(dataset_path)) {
            std::printf("[오류] 파일을 열 수 없습니다: %s\n", dataset_path);
            return 1;
        }

        auto end_build = std::chrono::high_resolution_clock::now();
        build_time_ms = std::chrono::duration<double, std::milli>(end_build - start_build).count();
    }
    else {
        // 파일 경로가 없으면 테스트를 위해 5,000만 개(약 400MB)의 정렬된 64비트 정수 데이터를 생성
        // (512MB 제한 내에 원본 배열 400MB + Radix 인덱스 16MB = 총 416MB로 안전하게 들어감)
        const uint32_t NUM_TEST_KEYS = 50'000'000;
        std::printf("[테스트 모드] 파일 인자가 없어 %u개(약 381MB)의 가상 정렬 데이터셋을 생성합니다...\n", NUM_TEST_KEYS);

        synthetic_data.resize(NUM_TEST_KEYS);
        uint64_t cur = 100;
        for (uint32_t i = 0; i < NUM_TEST_KEYS; ++i) {
            // 짝수/홀수 간격을 섞어 존재하는 키와 존재하지 않는 빈틈(Gap)을 생성
            cur += (i % 3 == 0) ? 5 : 2;
            synthetic_data[i] = cur;
        }

        PrintMemoryUsage();
        std::printf("[빌드 시작] 인덱스 구축 중...\n");

        // ★ 순수 자료구조 빌드 시간 측정 시작 ★
        auto start_build = std::chrono::high_resolution_clock::now();
        index.BuildFromArray(synthetic_data.data(), NUM_TEST_KEYS);
        auto end_build = std::chrono::high_resolution_clock::now();

        build_time_ms = std::chrono::duration<double, std::milli>(end_build - start_build).count();
    }

    std::printf("[빌드 완료] 총 원소 수: %u개 | 빌드 소요 시간: %.3f ms\n", index.size(), build_time_ms);
    PrintMemoryUsage();

    // ========================================================================
    // 4. 벤치마크: 존재하는 데이터(Hit) + 존재하지 않는 데이터(Miss) 검색 시간 측정
    // ========================================================================
    if (!synthetic_data.empty()) {
        const size_t NUM_QUERIES = 2'000'000; // 200만 개 쿼리 (100만 개 Hit + 100만 개 Miss)
        std::vector<uint64_t> queries(NUM_QUERIES);

        std::mt19937_64 rng(2022202090);
        std::uniform_int_distribution<uint32_t> idx_dist(0, index.size() - 1);

        for (size_t i = 0; i < NUM_QUERIES; ++i) {
            if (i % 2 == 0) {
                // 실제 존재하는 키 (Positive Query)
                queries[i] = synthetic_data[idx_dist(rng)];
            }
            else {
                // 실제 존재하지 않는 키 (Negative Query: 존재하는 키에서 1을 빼서 빈틈 값 생성)
                queries[i] = synthetic_data[idx_dist(rng)] - 1;
            }
        }

        std::printf("\n[벤치마크] 총 %zu개 쿼리(Hit 50%%, Miss 50%%) 검색 시작...\n", NUM_QUERIES);

        uint64_t hit_count = 0;
        uint64_t miss_count = 0;
        int64_t checksum = 0; // 컴파일러가 루프를 최적화로 지워버리는 것(Dead Code Elimination) 방지

        // ★ 실제 검색 시간 측정 시작 ★
        auto start_search = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < NUM_QUERIES; ++i) {
            int64_t pos = index.Search(queries[i]);
            if (pos != NOT_FOUND) {
                hit_count++;
                checksum ^= pos;
            }
            else {
                miss_count++;
            }
        }

        auto end_search = std::chrono::high_resolution_clock::now();
        double search_time_ms = std::chrono::duration<double, std::milli>(end_search - start_search).count();
        double avg_query_ns = (search_time_ms * 1'000'000.0) / static_cast<double>(NUM_QUERIES);

        std::printf("------------------------------------------------------------\n");
        std::printf(" ■ 검색 결과 요약 (Checksum: %lld)\n", checksum);
        std::printf("   - 존재하는 키 찾음 (Hit) : %llu 건\n", hit_count);
        std::printf("   - 없는 키 걸러냄 (Miss)  : %llu 건\n", miss_count);
        std::printf(" ■ 성능 측정 결과\n");
        std::printf("   - 빌드 소요 시간         : %.3f ms\n", build_time_ms);
        std::printf("   - 전체 검색 소요 시간    : %.3f ms (쿼리당 평균 %.2f ns)\n", search_time_ms, avg_query_ns);
        std::printf("   - ★ 총 합산 시간        : %.3f ms\n", build_time_ms + search_time_ms);
        std::printf("------------------------------------------------------------\n\n");
    }

    // ========================================================================
    // 5. 사용자로부터 직접 정수 키를 입력받아 위치를 출력하는 루프
    // ========================================================================
    std::printf("[대화형 검색 모드] 찾을 정수 키 값을 입력하세요 (종료하려면 음수 또는 q 입력):\n");
    while (true) {
        std::cout << "\nQuery Key > ";
        std::string input;
        if (!(std::cin >> input) || input == "q" || input == "Q" || input[0] == '-') {
            std::cout << "프로그램을 종료합니다.\n";
            break;
        }

        uint64_t query_key = 0;
        try {
            query_key = std::stoull(input);
        }
        catch (...) {
            std::cout << "유효한 양의 정수를 입력해 주세요.\n";
            continue;
        }

        auto t0 = std::chrono::high_resolution_clock::now();
        int64_t result_pos = index.Search(query_key);
        auto t1 = std::chrono::high_resolution_clock::now();
        double elapsed_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();

        if (result_pos != NOT_FOUND) {
            std::printf(" -> [존재함] 키 %llu 은(는) 데이터셋의 %lld 번째 위치(Index)에 존재합니다. (소요시간: %.1f ns)\n",
                query_key, result_pos, elapsed_ns);
        }
        else {
            std::printf(" -> [없음] 키 %llu 은(는) 데이터셋에 존재하지 않습니다. (소요시간: %.1f ns)\n",
                query_key, elapsed_ns);
        }
    }

    return 0;
}
