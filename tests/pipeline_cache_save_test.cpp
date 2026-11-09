// The actual save function, with authored cache bytes and Vulkan/file API spies. No driver is loaded.
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
constexpr int MOVEFILE_REPLACE_EXISTING = 1;
void Sleep(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#endif

// Arm only the allocating thread, after the query/copy API or immediately before thread construction.
// Real std::vector, std::string and std::thread allocations still run; nothing models their failure handling.
thread_local bool fail_next_new = false;
std::atomic<int> allocation_failures{0};
void* operator new(std::size_t n) {
    if (fail_next_new) {
        fail_next_new = false;
        ++allocation_failures;
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
// Keep the replacement allocator boundary: GCC must not inline free into a caller of operator new.
[[gnu::noinline]] void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

using VkResult = int;
constexpr VkResult VK_SUCCESS = 0, VK_ERROR_UNKNOWN = -13;
struct { int device = 1; } dev;
int pcache = 2;
std::string pcache_path;
std::atomic<int64_t> pcache_built_since_save{32};
enum Fault { NONE, VECTOR_ALLOC, VECTOR_LENGTH, TMP_ALLOC, QUERY_FAIL, EMPTY, COPY_FAIL, SHORT_WRITE, BLOCK_COPY };
std::atomic<Fault> fault{NONE};
std::atomic<int> queries{0}, copies{0}, writes{0}, replacements{0}, save_errors{0}, start_errors{0};
std::atomic<bool> copy_blocked{false}, allow_copy{false};

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

void logf(const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    std::fprintf(stderr, "%s\n", line);
    if (std::strstr(line, "pipeline cache save failed:")) ++save_errors;
    if (std::strstr(line, "could not start pipeline cache save:")) ++start_errors;
}

VkResult vkGetPipelineCacheData(int device, int cache, std::size_t* n, void* data) {
    if (device != dev.device || cache != pcache) std::abort();
    if (!data) {
        ++queries;
        if (fault == QUERY_FAIL) return VK_ERROR_UNKNOWN;
        *n = fault == VECTOR_LENGTH ? (std::numeric_limits<std::size_t>::max)() : fault == EMPTY ? 0 : 64;
        if (fault == VECTOR_ALLOC) fail_next_new = true;
    } else {
        ++copies;
        if (fault == COPY_FAIL) return VK_ERROR_UNKNOWN;
        if (fault == BLOCK_COPY) {
            copy_blocked = true;
            while (!allow_copy.load()) Sleep(1);
        }
        std::memset(data, 'N', *n);
        if (fault == TMP_ALLOC) fail_next_new = true;
    }
    return VK_SUCCESS;
}

std::size_t save_fwrite(const void* p, std::size_t size, std::size_t n, FILE* f) {
    ++writes;
    if (fault == SHORT_WRITE) return 0;
    return std::fwrite(p, size, n, f);
}
int save_replace(const char* from, const char* to, unsigned flags) {
    if (flags != MOVEFILE_REPLACE_EXISTING || to != pcache_path) std::abort();
    ++replacements;
#ifdef _WIN32
    return MoveFileExA(from, to, flags);
#else
    return std::rename(from, to) == 0;
#endif
}

#define fwrite save_fwrite
#define MoveFileExA save_replace
#include "pipeline_cache_save.inc"
#undef MoveFileExA
#undef fwrite

bool wait_until(const std::atomic<int>& n, int target) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (n.load() < target && std::chrono::steady_clock::now() < end) Sleep(1);
    return n.load() >= target;
}
bool old_file() {
    FILE* f = std::fopen(pcache_path.c_str(), "rb");
    if (!f) return false;
    char data[4]{};
    const auto n = std::fread(data, 1, sizeof data, f);
    std::fclose(f);
    return n == 3 && std::memcmp(data, "old", 3) == 0;
}
bool new_file() {
    FILE* f = std::fopen(pcache_path.c_str(), "rb");
    if (!f) return false;
    char data[65]{};
    const auto n = std::fread(data, 1, sizeof data, f);
    std::fclose(f);
    if (n != 64) return false;
    for (std::size_t i = 0; i < n; ++i) if (data[i] != 'N') return false;
    return true;
}

int run(const std::string& name) {
    if (name == "success") {
        pcache_built_since_save = 31;
        pipeline_cache_save(false);
        CHECK(queries == 0 && old_file());
        pcache_built_since_save = 32;
        pipeline_cache_save(false);
        CHECK(wait_until(copies, 1));
        pipeline_cache_save(true);  // Wait for the worker, then preserve the existing forced-save behavior.
        CHECK(queries == 2 && writes == 2 && replacements == 2 && new_file());
        CHECK(save_errors == 0 && start_errors == 0);
        pcache = 0;
        pipeline_cache_save(true);
        CHECK(queries == 2);
    } else if (name == "force-vector" || name == "async-vector" || name == "force-length" ||
               name == "force-string" || name == "async-string") {
        fault = name == "force-length" ? VECTOR_LENGTH : name.find("string") != std::string::npos ? TMP_ALLOC : VECTOR_ALLOC;
        const bool async = name.starts_with("async");
        pipeline_cache_save(!async);
        CHECK(wait_until(save_errors, 1));
        CHECK(old_file() && replacements == 0 && start_errors == 0);
        CHECK(allocation_failures == (fault == VECTOR_LENGTH ? 0 : 1));
        fault = NONE;
        pipeline_cache_save(true);
        CHECK(new_file() && replacements == 1 && queries == 2);  // No stuck busy flag after the failure.
    } else if (name == "thread-start") {
        fail_next_new = true;
        pipeline_cache_save(false);  // The real std::thread's state allocation, before it can run the worker.
        CHECK(!fail_next_new && allocation_failures == 1 && start_errors == 1 && save_errors == 0);
        CHECK(queries == 0 && old_file());
        pipeline_cache_save(true);
        CHECK(new_file() && queries == 1 && replacements == 1);
    } else if (name == "api-results") {
        for (Fault f : {QUERY_FAIL, EMPTY, COPY_FAIL}) {
            fault = f;
            pipeline_cache_save(true);
            CHECK(old_file() && replacements == 0 && save_errors == 0 && start_errors == 0);
        }
        fault = NONE;
        pipeline_cache_save(true);
        CHECK(new_file() && replacements == 1);
    } else if (name == "short-write") {
        fault = SHORT_WRITE;
        pipeline_cache_save(true);
        CHECK(writes == 1 && replacements == 0 && old_file());
        fault = NONE;
        pipeline_cache_save(true);
        CHECK(new_file() && replacements == 1);
    } else if (name == "forced-wait") {
        fault = BLOCK_COPY;
        pipeline_cache_save(false);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!copy_blocked.load() && std::chrono::steady_clock::now() < end) Sleep(1);
        CHECK(copy_blocked);
        std::atomic<bool> started{false}, finished{false};
        std::thread waiter([&] { started = true; pipeline_cache_save(true); finished = true; });
        while (!started.load()) Sleep(1);
        Sleep(20);
        const bool waited = !finished.load() && queries == 1;
        allow_copy = true;
        waiter.join();
        CHECK(waited && finished && queries == 2 && replacements == 2 && new_file());
    } else {
        CHECK(false);
    }
    std::printf("PASS: %s\n", name.c_str());
    return 0;
}

int main(int argc, char** argv) {
    // An old detached worker would terminate the process. The original-source negative runs in its own bounded child.
    std::set_terminate([] { std::fputs("uncaught worker exception\n", stderr); std::_Exit(86); });
    if (argc == 2 && std::strcmp(argv[1], "terminate-control") == 0) std::terminate();
    if (argc == 2 && std::strcmp(argv[1], "uncaught-thread-control") == 0) {
#ifdef _MSC_VER
        std::fputs("MSVC-target uncaught-thread control\n", stderr);
#endif
        std::thread worker([] { throw std::bad_alloc(); });
        worker.join();
        return 2;
    }
    if (argc != 3) return 2;
    pcache_path = std::string(argv[2]) + "/authored-pipeline-cache-with-a-path-longer-than-small-string-storage.bin";
    FILE* f = std::fopen(pcache_path.c_str(), "wb");
    if (!f) return 2;
    std::fwrite("old", 1, 3, f);
    std::fclose(f);
    try {
        return run(argv[1]);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "uncaught caller exception: %s\n", e.what());
        return 87;
    }
}
