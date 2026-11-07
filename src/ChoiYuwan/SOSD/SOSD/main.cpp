// C++14, Windows x64. SOSD: uint64 count + sorted uint32 keys.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#define NOMINMAX
#include <windows.h>
using Clock = std::chrono::steady_clock;
static double seconds(Clock::time_point a, Clock::time_point b) {
	return std::chrono::duration<double>(b - a).count();
}
// Remains alive for the lifetime of the search program.
class MemoryLimit {
	HANDLE job = nullptr;
public:
	MemoryLimit() {
		job = CreateJobObjectW(nullptr, nullptr);
		if (!job) throw std::runtime_error(u8"메모리 제한용 Job Object를 생성하지 못했습니다.");
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
		info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
		info.ProcessMemoryLimit = SIZE_T(512) * 1024 * 1024;
		if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)) ||
			!AssignProcessToJobObject(job, GetCurrentProcess())) {
			DWORD code = GetLastError(); CloseHandle(job); job = nullptr;
			throw std::runtime_error(u8"512 MiB 메모리 제한 설정에 실패했습니다. Windows 오류 코드: " + std::to_string(code));
		}
	}
	~MemoryLimit() {
		if (job) CloseHandle(job);
	}
};
class Searcher {
	// One sample per 1 MiB; one cached block holds 4 KiB.
	static constexpr uint64_t RegionKeys = 262144;
	static constexpr uint64_t BlockKeys = 1024;
	static constexpr size_t CacheSlots = 16384; // Maximum 64 MiB
	std::ifstream file;
	uint64_t count = 0;
	uint32_t firstValue = 0;
	std::vector<uint32_t> maxima;
	std::unique_ptr<uint32_t[]> cache;
	std::vector<uint64_t> tags;

	void readAt(uint64_t offset, void* destination, size_t bytes) {
		file.clear();
		file.seekg(static_cast<std::streamoff>(offset));
		if (!file.read(static_cast<char*>(destination), static_cast<std::streamsize>(bytes)))
			throw std::runtime_error(u8"파일 읽기에 실패했습니다. 실행 중에는 데이터 파일을 변경하지 마세요.");
	}
	uint32_t readKey(uint64_t position) {
		uint32_t value;
		readAt(8 + position * 4, &value, sizeof(value));
		return value;
	}
	const uint32_t* getBlock(uint64_t block) {
		size_t slot = static_cast<size_t>(block % CacheSlots);
		uint32_t* destination = cache.get() + slot * BlockKeys;
		if (tags[slot] == block) {
			++cacheHits;
		}
		else {
			uint64_t length = std::min<uint64_t>(BlockKeys, count - block * BlockKeys);
			readAt(8 + block * BlockKeys * 4, destination, static_cast<size_t>(length * 4));
			tags[slot] = block;
			++blockReads;
		}
		return destination;
	}
public:
	uint64_t blockReads = 0, cacheHits = 0;
	uint64_t interpolationSteps = 0, binarySteps = 0;

	explicit Searcher(const std::string& path)
		: file(path, std::ios::binary) {
		if (!file) throw std::runtime_error(u8"데이터 파일을 열 수 없습니다: " + path);
		file.seekg(0, std::ios::end);
		std::streampos end = file.tellg();
		if (end < std::streampos(8)) throw std::runtime_error(u8"SOSD 파일 헤더가 없거나 올바르지 않습니다.");
		uint64_t bytes = static_cast<uint64_t>(static_cast<std::streamoff>(end));
		readAt(0, &count, sizeof(count));
		if ((bytes - 8) % 4 != 0 || count != (bytes - 8) / 4)
			throw std::runtime_error(u8"압축을 푼 SOSD uint32 파일이 필요합니다. 원소 개수가 저장된 uint64 헤더와 파일 크기를 확인하세요.");
		// Also bounds the 64-bit product used in interpolation.
		if (count > std::numeric_limits<uint32_t>::max())
			throw std::runtime_error(u8"이 프로그램은 최대 4,294,967,295개의 원소를 지원합니다.");
		if (count != 0) {
			firstValue = readKey(0);
			uint64_t regions = (count + RegionKeys - 1) / RegionKeys;
			maxima.reserve(static_cast<size_t>(regions));
			uint32_t previous = firstValue;
			for (uint64_t region = 0; region < regions; ++region) {
				uint64_t last = std::min<uint64_t>(count, (region + 1) * RegionKeys) - 1;
				uint32_t value = readKey(last);
				if (value < previous)
					throw std::runtime_error(u8"표본 값이 정렬되어 있지 않습니다. 오름차순으로 정렬된 SOSD 데이터가 필요합니다.");
				maxima.push_back(value);
				previous = value;
			}
		}
		// No value-initialization of 64 MiB at startup. A slot is read only after filling it.
		cache.reset(new uint32_t[CacheSlots * BlockKeys]);
		tags.assign(CacheSlots, std::numeric_limits<uint64_t>::max());
	}
	uint64_t size() const { return count; }
	size_t samples() const { return maxima.size(); }

	// Exact lower_bound search: first occurrence, zero-based; -1 if absent.
	int64_t find(uint32_t key) {
		if (count == 0 || key < firstValue) return -1;
		if (key == firstValue) return 0;
		std::vector<uint32_t>::const_iterator sample =
			std::lower_bound(maxima.cbegin(), maxima.cend(), key);
		if (sample == maxima.cend()) return -1;
		uint64_t region = static_cast<uint64_t>(sample - maxima.cbegin());

		// Invariant: A[low] < key <= A[high]. The first occurrence is in (low, high].
		uint64_t low = region == 0 ? 0 : region * RegionKeys - 1;
		uint64_t high = std::min<uint64_t>(count, (region + 1) * RegionKeys) - 1;
		uint32_t lowValue = region == 0 ? firstValue : maxima[static_cast<size_t>(region - 1)];
		uint32_t highValue = *sample;
		bool interpolate = true;
		unsigned attempts = 0;
		while (high - low > 1) {
			uint64_t oldWidth = high - low;
			uint64_t position;
			if (interpolate && attempts < 4) {
				// lowValue < key <= highValue: denominator is strictly positive.
				uint64_t numerator = uint64_t(key - lowValue) * oldWidth;
				position = low + numerator / uint64_t(highValue - lowValue);
				position = std::max<uint64_t>(low + 1, std::min<uint64_t>(position, high - 1));
				++attempts;
				++interpolationSteps;
			}
			else {
				position = low + oldWidth / 2;
				++binarySteps;
			}
			uint64_t block = position / BlockKeys;
			uint64_t base = block * BlockKeys;
			const uint32_t* data = getBlock(block);
			uint64_t begin = std::max<uint64_t>(low + 1, base);
			uint64_t end = std::min<uint64_t>(high + 1, std::min<uint64_t>(count, base + BlockKeys));
			const uint32_t* first = data + (begin - base);
			const uint32_t* last = data + (end - base);
			const uint32_t* found = std::lower_bound(first, last, key);
			if (found == last) {
				low = end - 1;
				lowValue = *(last - 1);
			}
			else {
				uint64_t foundPosition = base + static_cast<uint64_t>(found - data);
				// A predecessor inside this slice proves every earlier element is smaller.
				if (found != first)
					return *found == key ? static_cast<int64_t>(foundPosition) : -1;
				high = foundPosition;
				highValue = *found;
			}
			// Poor progress disables interpolation for the remainder of this query.
			if (high - low > oldWidth * 3 / 4) interpolate = false;
		}
		return highValue == key ? static_cast<int64_t>(high) : -1;
	}
};
constexpr uint64_t Searcher::RegionKeys;
constexpr uint64_t Searcher::BlockKeys;
constexpr size_t Searcher::CacheSlots;

// Parse digits directly, rejecting negatives, non-digits and uint32 overflow.
static bool parseKey(const std::string& s, uint32_t& key) {
	if (s.empty()) return false;
	uint32_t value = 0;
	const uint32_t maximum = std::numeric_limits<uint32_t>::max();
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] < '0' || s[i] > '9') return false;
		uint32_t digit = static_cast<uint32_t>(s[i] - '0');
		if (value > (maximum - digit) / 10) return false;
		value = value * 10 + digit;
	}
	key = value;
	return true;
}
// Accept a pasted path, including Explorer's surrounding quotes.
static std::string cleanPath(const std::string& text) {
	size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return "";
	size_t last = text.find_last_not_of(" \t\r\n");
	std::string path = text.substr(first, last - first + 1);
	if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
		path = path.substr(1, path.size() - 2);
	return path;
}
int main() {
	// Print UTF-8 Korean text correctly in the Windows console.
	SetConsoleOutputCP(CP_UTF8);
	try {
		MemoryLimit limit;
		std::cout << u8"SOSD 검색 프로그램 (Windows x64, 메모리 제한: 512 MiB)\n"
			<< u8"압축을 푼 오름차순 SOSD uint32 데이터 파일을 사용하세요.\n";
		std::unique_ptr<Searcher> search;
		double build = 0;
		while (!search) {
			std::cout << u8"\n데이터 파일의 전체 경로를 입력하세요. (종료: q)\n> " << std::flush;
			std::string line;
			if (!std::getline(std::cin, line)) return 0;
			std::string path = cleanPath(line);
			if (path == "q" || path == "Q") return 0;
			if (path.empty()) continue;
			try {
				Clock::time_point start = Clock::now();
				search.reset(new Searcher(path));
				build = seconds(start, Clock::now());
			}
			catch (const std::exception& error) {
				std::cout << u8"데이터를 불러올 수 없습니다: " << error.what()
					<< u8"\n파일 경로를 확인한 뒤 다시 입력하세요.\n";
			}
		}
		std::cout << std::fixed << std::setprecision(9)
			<< u8"준비 완료. 전체 원소 수: " << search->size() << u8"개 / 표본 수: " << search->samples()
			<< u8"개 / 구축 시간: " << build << u8" 초\n"
			<< u8"검색할 정수를 입력하세요. (0~4294967295, 종료: q)\n";
		uint64_t hits = 0, misses = 0;
		double hitTime = 0, missTime = 0;
		std::string token;
		while (true) {
			std::cout << "> " << std::flush;
			if (!(std::cin >> token) || token == "q" || token == "Q") break;
			uint32_t key;
			if (!parseKey(token, key)) {
				std::cout << u8"잘못된 입력입니다. 0~4294967295 범위의 정수 또는 종료하려면 q를 입력하세요.\n";
				continue;
			}
			Clock::time_point start = Clock::now();
			int64_t position = search->find(key);
			double elapsed = seconds(start, Clock::now());
			if (position >= 0) {
				++hits;
				hitTime += elapsed;
				std::cout << u8"검색 성공! 인덱스(0부터): " << position << u8" / 순번(1부터): " << position + 1;
			}
			else {
				++misses;
				missTime += elapsed;
				std::cout << u8"검색한 값이 데이터에 없습니다.";
			}
			std::cout << u8" / 검색 시간: " << elapsed * 1000000 << u8" 마이크로초(μs)\n";
		}
		if (std::cin.bad()) throw std::runtime_error(u8"입력을 읽는 중 오류가 발생했습니다.");
		std::cout << u8"\n--- 검색 결과 ---\n"
			<< u8"검색 성공 횟수: " << hits << u8"회 / 검색 실패 횟수: " << misses << u8"회\n"
			<< u8"구축 시간: " << build << u8" 초\n"
			<< u8"성공한 검색의 누적 시간: " << hitTime << u8" 초 / 실패한 검색의 누적 시간: " << missTime << u8" 초\n"
			<< u8"전체 검색 시간: " << hitTime + missTime << u8" 초\n"
			<< u8"구축 시간 + 전체 검색 시간: " << build + hitTime + missTime << u8" 초\n"
			<< u8"파일에서 블록을 읽은 횟수: " << search->blockReads << u8"회 / 캐시를 재사용한 횟수: " << search->cacheHits << u8"회\n";
	}
	catch (const std::exception& error) {
		std::cerr << u8"오류: " << error.what() << '\n';
		return 1;
	}
	return 0;
}
