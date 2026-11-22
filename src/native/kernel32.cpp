// KERNEL32 for the native host: the subset SporeApp.exe imports.
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "win.h"

namespace native {

std::mutex g_kernel_mutex;
std::condition_variable g_kernel_cv;
LoadedImage g_engine;
const char* const kGuestGameDir = "C:\\Game";
const char* const kGuestExeDir = "C:\\Game\\SporebinEP1";

namespace {

// ---- handles ----
// handle = 0x100 + 4 * index. Lookups (every wait and signal) read the table without a lock; slots are assigned
// and cleared under g_handles_mutex.
constexpr uint32_t kMaxHandles = 1u << 16;
std::mutex g_handles_mutex;
std::atomic<KObject*> g_handles[kMaxHandles];
uint32_t g_handles_used = 0;  // slots below this have been handed out at least once

thread_local uint32_t tl_last_error = 0;

}  // namespace

uint32_t NewHandle(KObject* obj) {
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    uint32_t i = 0;
    while (i < g_handles_used && g_handles[i].load(std::memory_order_relaxed)) ++i;
    if (i == kMaxHandles) Fatal("out of kernel handles");
    if (i == g_handles_used) ++g_handles_used;
    g_handles[i].store(obj, std::memory_order_release);
    return 0x100 + 4 * i;
}

KObject* HandleObject(uint32_t handle) {
    if (handle < 0x100 || (handle & 3)) return nullptr;
    uint32_t i = (handle - 0x100) / 4;
    return i < kMaxHandles ? g_handles[i].load(std::memory_order_acquire) : nullptr;
}

void CloseKernelHandle(uint32_t handle) {
    KObject* obj = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_handles_mutex);
        if (handle < 0x100 || (handle & 3)) return;
        uint32_t i = (handle - 0x100) / 4;
        if (i >= kMaxHandles) return;
        obj = g_handles[i].exchange(nullptr);
    }
    if (obj && --obj->refs == 0 && obj->type != KType::Thread) delete obj;
}

void SetLastError(uint32_t code) { tl_last_error = code; }
uint32_t GetLastError() { return tl_last_error; }

// ---- paths ----
namespace {

std::mutex g_cwd_mutex;
std::string g_cwd = kGuestExeDir;  // the GOG shortcut starts the game in its bin folder

std::string HostRootFor(char drive) {
    // C:\Game is the game directory; everything else on C: (saves, creations, settings, temp files) lives in
    // g_user_root: on Android <game>/UserData when the game folder is writable (main.cpp PrepareUserRoot), else the
    // app's data folder; native_c next to the game folder elsewhere.
    (void)drive;
    if (g_user_root) return g_user_root;
    return g_data_dir ? std::string(g_data_dir) + "/native_c" : std::string(g_game_dir) + "/../native_c";
}

// Finds `name` in `dir` ignoring case; returns the real name or `name` itself when nothing matches.
std::string MatchCase(const std::string& dir, const std::string& name) {
    struct stat st;
    if (stat((dir + "/" + name).c_str(), &st) == 0) return name;
    DIR* d = opendir(dir.c_str());
    if (!d) return name;
    std::string found = name;
    while (dirent* e = readdir(d)) {
        if (strcasecmp(e->d_name, name.c_str()) == 0) {
            found = e->d_name;
            break;
        }
    }
    closedir(d);
    return found;
}

std::string Normalize(const std::string& path) {
    // Absolute Windows path with "." and ".." resolved, backslashes.
    std::vector<std::string> parts;
    std::string drive = path.substr(0, 2);
    size_t i = 2;
    while (i <= path.size()) {
        size_t j = path.find_first_of("\\/", i);
        if (j == std::string::npos) j = path.size();
        std::string p = path.substr(i, j - i);
        if (p == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!p.empty() && p != ".") {
            parts.push_back(p);
        }
        i = j + 1;
    }
    std::string r = drive;
    for (auto& p : parts) r += "\\" + p;
    if (parts.empty()) r += "\\";
    return r;
}

}  // namespace

std::string GuestCurrentDir() {
    std::lock_guard<std::mutex> lock(g_cwd_mutex);
    return g_cwd;
}

void SetGuestCurrentDir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(g_cwd_mutex);
    g_cwd = dir;
}

std::string FullGuestPath(const char* win_path) {
    std::string p(win_path);
    if (p.size() >= 2 && p[1] == ':') return Normalize(p);
    if (!p.empty() && (p[0] == '\\' || p[0] == '/')) return Normalize(GuestCurrentDir().substr(0, 2) + p);
    return Normalize(GuestCurrentDir() + "\\" + p);
}

std::string HostPath(const char* win_path) {
    std::string full = FullGuestPath(win_path);
    std::string rest;
    std::string host;
    std::string game = kGuestGameDir;
    if (strncasecmp(full.c_str(), game.c_str(), game.size()) == 0 &&
        (full.size() == game.size() || full[game.size()] == '\\')) {
        host = g_game_dir;
        rest = full.substr(game.size());
    } else {
        host = HostRootFor(full[0]);
        rest = full.substr(2);
    }
    size_t i = 0;
    while (i < rest.size()) {
        if (rest[i] == '\\') {
            ++i;
            continue;
        }
        size_t j = rest.find('\\', i);
        if (j == std::string::npos) j = rest.size();
        host += "/" + MatchCase(host, rest.substr(i, j - i));
        i = j;
    }
    return host;
}

// Read statistics for the perf line: the engine streams music and speech from the archives with synchronous reads
// on its I/O thread, so slow storage (shared storage goes through FUSE) shows up as audio dropouts.
struct IoStats {
    std::atomic<uint64_t> reads{0}, bytes{0}, slow{0}, max_us{0}, total_us{0};
    std::atomic<uint64_t> by_size[4]{};  // reads under 4 KiB, under 64 KiB, under 1 MiB, larger
} g_io;

// "N reads, M MiB, S over 50 ms, longest X ms" since the last call.
std::string TakeIoStats() {
    char line[224];
    unsigned long long s0 = g_io.by_size[0].exchange(0), s1 = g_io.by_size[1].exchange(0),
                       s2 = g_io.by_size[2].exchange(0), s3 = g_io.by_size[3].exchange(0);
    snprintf(line, sizeof(line),
             "%llu reads (<4K %llu, <64K %llu, <1M %llu, more %llu), %llu KiB, %.0f ms reading, %llu over 50 ms, "
             "longest %.1f ms",
             (unsigned long long)g_io.reads.exchange(0), s0, s1, s2, s3,
             (unsigned long long)(g_io.bytes.exchange(0) >> 10),
             double(g_io.total_us.exchange(0)) / 1000.0, (unsigned long long)g_io.slow.exchange(0),
             double(g_io.max_us.exchange(0)) / 1000.0);
    return line;
}

namespace {

// ---- waitable objects ----
struct Event : KObject {
    Event(bool manual, bool state) : KObject(KType::Event), manual(manual), state(state) {}
    bool Signaled(uint32_t) override { return state; }
    void Acquire(uint32_t) override {
        if (!manual) state = false;
    }
    bool manual, state;
};

struct Semaphore : KObject {
    Semaphore(int32_t count, int32_t max) : KObject(KType::Semaphore), count(count), max(max) {}
    bool Signaled(uint32_t) override { return count > 0; }
    void Acquire(uint32_t) override { --count; }
    int32_t count, max;
};

struct Mutex : KObject {
    Mutex() : KObject(KType::Mutex) {}
    bool Signaled(uint32_t tid) override { return owner == 0 || owner == tid; }
    void Acquire(uint32_t tid) override {
        owner = tid;
        ++recursion;
    }
    uint32_t owner = 0;
    int recursion = 0;
};

struct Thread : KObject {
    Thread() : KObject(KType::Thread) {}
    bool Signaled(uint32_t) override { return done; }
    bool done = false;
    bool suspended = false;
    uint32_t exit_code = 259;  // STILL_ACTIVE
    uint32_t id = 0;
    int host_tid = 0;
};

uint32_t CurrentTid() { return CurrentThread()->id; }

// Waits until `pred` holds or the timeout expires (0xffffffff = forever); g_kernel_mutex is held by `lock`. The
// thread sleeps on its own condition variable, registered with each object it waits for, so a signal wakes only
// that object's waiters (the engine's job system signals and waits tens of thousands of times a second).
template <typename Pred>
bool WaitKernel(std::unique_lock<std::mutex>& lock, uint32_t ms, KObject* const* objs, size_t n, Pred pred) {
    if (pred()) return true;
    if (ms == 0) return false;
    thread_local KWaiter self;
    for (size_t i = 0; i < n; ++i)
        if (objs[i]) objs[i]->waiters.push_back(&self);
    bool ok;
    if (ms == 0xffffffffu) {
        self.cv.wait(lock, pred);
        ok = true;
    } else {
        ok = self.cv.wait_for(lock, std::chrono::milliseconds(ms), pred);
    }
    for (size_t i = 0; i < n; ++i) {
        if (!objs[i]) continue;
        auto& w = objs[i]->waiters;
        for (size_t k = 0; k < w.size(); ++k) {
            if (w[k] == &self) {
                w[k] = w.back();
                w.pop_back();
                break;
            }
        }
    }
    return ok;
}

// ---- files ----
// Files opened for reading only get a block cache: the game reads its packages in many small pieces, and on a
// phone shared storage (/sdcard) goes through FUSE, where every read() is a round trip to another process. Such
// files keep their own position (pos); every read and seek of them goes through ReadFile/SetFilePointer here.
constexpr uint32_t kReadBlock = 512 * 1024;
struct File : KObject {
    File() : KObject(KType::File) {}
    ~File() override {
        if (fd >= 0) close(fd);
    }
    int fd = -1;
    bool cached = false;
    std::mutex mu;
    int64_t pos = 0;
    std::vector<uint8_t> block;
    int64_t block_off = -1;
    size_t block_len = 0;

    // pread through the block cache (mu held). Returns bytes read or -1.
    ssize_t CachedRead(uint8_t* dst, size_t n, int64_t off) {
        if (n >= kReadBlock) return pread(fd, dst, n, off_t(off));
        size_t done = 0;
        while (done < n) {
            int64_t at = off + int64_t(done);
            if (block_off < 0 || at < block_off || at >= block_off + int64_t(block_len)) {
                if (block.empty()) block.resize(kReadBlock);
                block_off = at & ~int64_t(kReadBlock - 1);
                ssize_t r = pread(fd, block.data(), kReadBlock, off_t(block_off));
                if (r < 0) {
                    block_off = -1;
                    return done ? ssize_t(done) : -1;
                }
                block_len = size_t(r);
                if (at >= block_off + int64_t(block_len)) break;  // end of file
            }
            size_t k = std::min(n - done, size_t(block_off + int64_t(block_len) - at));
            memcpy(dst + done, block.data() + (at - block_off), k);
            done += k;
        }
        return ssize_t(done);
    }
};

struct Find : KObject {
    Find() : KObject(KType::Find) {}
    std::string dir, pattern;
    std::vector<std::string> names;
    size_t next = 0;
};

constexpr uint32_t ERROR_NOT_SUPPORTED = 50, ERROR_ENVVAR_NOT_FOUND = 203, ERROR_RESOURCE_TYPE_NOT_FOUND = 1813;
constexpr uint32_t ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_ACCESS_DENIED = 5,
                   ERROR_INVALID_HANDLE = 6, ERROR_NO_MORE_FILES = 18, ERROR_ALREADY_EXISTS = 183,
                   ERROR_FILE_EXISTS = 80;

uint32_t ErrnoToWin(int e) {
    switch (e) {
        case ENOENT: return ERROR_FILE_NOT_FOUND;
        case ENOTDIR: return ERROR_PATH_NOT_FOUND;
        case EACCES: case EPERM: return ERROR_ACCESS_DENIED;
        case EEXIST: return ERROR_ALREADY_EXISTS;
        default: return 31;  // ERROR_GEN_FAILURE
    }
}

// 100 ns intervals since 1601-01-01.
uint64_t ToFileTime(const struct timespec& ts) {
    return (uint64_t(ts.tv_sec) + 11644473600ull) * 10000000ull + uint64_t(ts.tv_nsec) / 100;
}

uint32_t Attributes(const struct stat& st) {
    uint32_t a = S_ISDIR(st.st_mode) ? 0x10 : 0x80;  // DIRECTORY : NORMAL
    if (!(st.st_mode & S_IWUSR)) a |= 0x01;           // READONLY
    return a;
}

void FillFindData(uint32_t out, const std::string& dir, const std::string& name) {
    memset(Ptr<void>(out), 0, 320);
    struct stat st;
    if (stat((dir + "/" + name).c_str(), &st) == 0) {
        W32(out, Attributes(st));
        uint64_t ft = ToFileTime(st.st_mtim);
        W64(out + 4, ft);
        W64(out + 12, ft);
        W64(out + 20, ft);
        W32(out + 28, uint32_t(uint64_t(st.st_size) >> 32));
        W32(out + 32, uint32_t(st.st_size));
    }
    strncpy(Str(out + 44), name.c_str(), 259);
}

bool FindNextMatch(Find* f, uint32_t out) {
    while (f->next < f->names.size()) {
        const std::string& n = f->names[f->next++];
        if (fnmatch(f->pattern.c_str(), n.c_str(), FNM_CASEFOLD) == 0) {
            FillFindData(out, f->dir, n);
            return true;
        }
    }
    return false;
}

// ---- modules ----
std::mutex g_modules_mutex;
std::vector<std::string> g_module_names;  // fake module handle = 0x0e000000 + 0x10000 * index
constexpr uint32_t kModuleBase = 0x0e000000u;
constexpr uint32_t kExeBase = 0x00400000u;

std::string LowerName(const char* s) {
    std::string r(s);
    for (char& ch : r) ch = char(tolower((unsigned char)ch));
    size_t slash = r.find_last_of("\\/");
    if (slash != std::string::npos) r = r.substr(slash + 1);
    if (r.find('.') == std::string::npos) r += ".dll";
    return r;
}

uint32_t ModuleHandle(const std::string& name) {
    if (name == "sporeapp.exe") return g_engine.base;
    std::lock_guard<std::mutex> lock(g_modules_mutex);
    for (size_t i = 0; i < g_module_names.size(); ++i) {
        if (g_module_names[i] == name) return kModuleBase + 0x10000 * uint32_t(i);
    }
    g_module_names.push_back(name);
    return kModuleBase + 0x10000 * uint32_t(g_module_names.size() - 1);
}

// Libraries the host provides (LoadLibrary succeeds for these only).
bool KnownLibrary(const std::string& name) {
    static const char* known[] = {"kernel32.dll", "user32.dll", "advapi32.dll", "msvcr90.dll", "d3d9.dll",
                                  "d3dx9_27.dll", "dsound.dll", "dinput8.dll", "winmm.dll", "ws2_32.dll",
                                  "wsock32.dll", "shell32.dll", "ole32.dll", "gdi32.dll", "imm32.dll", "usp10.dll",
                                  "version.dll", "psapi.dll", "dbghelp.dll", "steam_api.dll", "sporeapp.exe"};
    for (const char* k : known)
        if (name == k) return true;
    return false;
}

auto g_start = std::chrono::steady_clock::now();

uint64_t NowFileTime() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ToFileTime(ts);
}

void WriteSystemTime(uint32_t out, const struct tm& t, int ms) {
    W16(out + 0, uint16_t(t.tm_year + 1900));
    W16(out + 2, uint16_t(t.tm_mon + 1));
    W16(out + 4, uint16_t(t.tm_wday));
    W16(out + 6, uint16_t(t.tm_mday));
    W16(out + 8, uint16_t(t.tm_hour));
    W16(out + 10, uint16_t(t.tm_min));
    W16(out + 12, uint16_t(t.tm_sec));
    W16(out + 14, uint16_t(ms));
}

// ---- critical sections: host recursive mutexes keyed by the guest CRITICAL_SECTION address ----
// The address map decides which mutex a CRITICAL_SECTION gets (DeleteCriticalSection keeps it). Its index is also
// put, tagged, into the struct's LockSemaphore field (offset 16, an opaque handle the game never reads), so Enter/
// Leave read one field and index an array instead of hashing the address on every call.
constexpr uint32_t kCsTag = 0xc5400000u, kCsTagMask = 0xfffc0000u, kCsMax = 1u << 18;
std::mutex g_cs_mutex;
std::unordered_map<uint32_t, uint32_t> g_cs;  // guest address -> index in g_cs_table
std::recursive_mutex* g_cs_table[kCsMax];
uint32_t g_cs_count = 0;
std::unordered_map<uint32_t, std::recursive_mutex*> g_cs_overflow;

std::recursive_mutex* CriticalSectionSlow(uint32_t cs) {
    std::lock_guard<std::mutex> lock(g_cs_mutex);
    auto it = g_cs.find(cs);
    if (it == g_cs.end()) {
        if (g_cs_count >= kCsMax) {
            auto& m = g_cs_overflow[cs];
            if (!m) m = new std::recursive_mutex();
            return m;
        }
        g_cs_table[g_cs_count] = new std::recursive_mutex();
        it = g_cs.emplace(cs, g_cs_count++).first;
    }
    W32(cs + 16, kCsTag | it->second);
    return g_cs_table[it->second];
}

std::recursive_mutex* CriticalSection(uint32_t cs) {
    uint32_t t = R32(cs + 16);
    if ((t & kCsTagMask) == kCsTag) {
        // Null while another thread's store of the table entry isn't visible yet: the slow path takes the lock.
        std::recursive_mutex* m = __atomic_load_n(&g_cs_table[t & ~kCsTagMask], __ATOMIC_ACQUIRE);
        if (m) return m;
    }
    return CriticalSectionSlow(cs);
}

// ---- TLS ----
std::mutex g_tls_mutex;
bool g_tls_used[64];

#define K(name) void k_##name(Ctx* c)

// ---------------- handlers ----------------
K(GetCurrentProcessId) { Return(c, 0x1000); }
K(GetCurrentThreadId) { Return(c, CurrentTid()); }
K(GetCurrentProcess) { Return(c, kCurrentProcess); }
K(GetCurrentThread) { Return(c, kCurrentThread); }
K(GetTickCount) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start);
    Return(c, uint32_t(ms.count()));
}
K(QueryPerformanceCounter) {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_start);
    W64(Arg(c, 0), uint64_t(ns.count() / 100));
    Return(c, 1, 1);
}
K(QueryPerformanceFrequency) {
    W64(Arg(c, 0), 10000000ull);
    Return(c, 1, 1);
}
K(GetSystemTimeAsFileTime) {
    W64(Arg(c, 0), NowFileTime());
    Return(c, 0, 1);
}
K(GetLocalTime) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm t;
    localtime_r(&tv.tv_sec, &t);
    WriteSystemTime(Arg(c, 0), t, int(tv.tv_usec / 1000));
    Return(c, 0, 1);
}
K(GetTimeZoneInformation) {
    memset(Ptr<void>(Arg(c, 0)), 0, 172);
    Return(c, 0, 1);  // TIME_ZONE_ID_UNKNOWN, bias 0
}
K(InterlockedCompareExchange) {
    uint32_t expected = Arg(c, 2);
    __atomic_compare_exchange_n(Ptr<uint32_t>(Arg(c, 0)), &expected, Arg(c, 1), false, __ATOMIC_SEQ_CST,
                                __ATOMIC_SEQ_CST);
    Return(c, expected, 3);
}
K(InterlockedExchange) { Return(c, __atomic_exchange_n(Ptr<uint32_t>(Arg(c, 0)), Arg(c, 1), __ATOMIC_SEQ_CST), 2); }
K(InterlockedDecrement) { Return(c, __atomic_sub_fetch(Ptr<uint32_t>(Arg(c, 0)), 1, __ATOMIC_SEQ_CST), 1); }

K(VirtualAlloc) {
    uint32_t addr = Arg(c, 0), size = Arg(c, 1);
    uint32_t r = PageAlloc(size, addr);
    if (!r) SetLastError(8);  // ERROR_NOT_ENOUGH_MEMORY
    Return(c, r, 4);
}
K(VirtualFree) {
    if (Arg(c, 2) & 0x8000) PageFree(Arg(c, 0));  // MEM_RELEASE; MEM_DECOMMIT keeps the pages
    Return(c, 1, 3);
}
K(GetProcessHeap) { Return(c, 0x00ee0000u); }
K(HeapFree) {
    native::HeapFree(Arg(c, 2));
    Return(c, 1, 3);
}
K(GlobalMemoryStatus) {
    uint32_t m = Arg(c, 0);
    W32(m + 0, 32);
    W32(m + 4, 30);
    W32(m + 8, 0x7fffffffu);   // total physical
    W32(m + 12, 0x60000000u);  // available physical
    W32(m + 16, 0x7fffffffu);
    W32(m + 20, 0x60000000u);
    W32(m + 24, 0x7ffe0000u);  // total virtual
    W32(m + 28, 0x60000000u);
    Return(c, 0, 1);
}

K(GetModuleHandleA) {
    uint32_t name = Arg(c, 0);
    Return(c, name ? ModuleHandle(LowerName(Str(name))) : kExeBase, 1);
}
K(LoadLibraryA) {
    std::string n = LowerName(Str(Arg(c, 0)));
    if (!KnownLibrary(n)) {
        Log("LoadLibraryA(%s): not provided", Str(Arg(c, 0)));
        SetLastError(126);  // ERROR_MOD_NOT_FOUND
        Return(c, 0, 1);
        return;
    }
    Return(c, ModuleHandle(n), 1);
}
K(FreeLibrary) { Return(c, 1, 1); }
K(GetProcAddress) {
    uint32_t module = Arg(c, 0), name = Arg(c, 1);
    std::string proc = name < 0x10000 ? "#" + std::to_string(name) : std::string(Str(name));
    uint32_t r = 0;
    if (module == g_engine.base) {
        r = FindExport(g_engine, proc.c_str());
    } else if (module >= kModuleBase && module < kModuleBase + 0x1000000) {
        std::string dll;
        {
            std::lock_guard<std::mutex> lock(g_modules_mutex);
            dll = g_module_names[(module - kModuleBase) / 0x10000];
        }
        r = HasImport(dll.c_str(), proc.c_str()) ? ImportThunk(dll.c_str(), proc.c_str()) : 0;
        if (!r) Log("GetProcAddress(%s, %s): not provided", dll.c_str(), proc.c_str());
    }
    Return(c, r, 2);
}
K(GetModuleFileNameA) {
    uint32_t module = Arg(c, 0), buf = Arg(c, 1), size = Arg(c, 2);
    std::string p = std::string(kGuestExeDir) + "\\SporeApp.exe";
    uint32_t n = uint32_t(p.size()) < size ? uint32_t(p.size()) : size - 1;
    memcpy(Str(buf), p.c_str(), n);
    Str(buf)[n] = 0;
    Return(c, n, 3);
}

K(GetVersionExA) {
    uint32_t v = Arg(c, 0);
    W32(v + 4, 6);      // Windows 7
    W32(v + 8, 1);
    W32(v + 12, 7601);
    W32(v + 16, 2);     // VER_PLATFORM_WIN32_NT
    strcpy(Str(v + 20), "Service Pack 1");
    Return(c, 1, 1);
}
K(IsDebuggerPresent) { Return(c, 0); }
K(SetErrorMode) { Return(c, 0, 1); }
K(SetUnhandledExceptionFilter) { Return(c, 0, 1); }
K(UnhandledExceptionFilter) { Return(c, 0, 1); }
K(OutputDebugStringA) {
    Log("debug: %s", Str(Arg(c, 0)));
    Return(c, 0, 1);
}
K(RaiseException) {
    uint32_t code = Arg(c, 0);
    if (code == 0x406d1388u) {  // MSVC SetThreadName convention: ignored
        Return(c, 0, 4);
        return;
    }
    Fatal("RaiseException %08x from %08x", code, R32(c->esp));
}
K(TerminateProcess) {
    Log("TerminateProcess(%u)", Arg(c, 1));
    _exit(int(Arg(c, 1)));
}
K(GetLastError) { Return(c, GetLastError()); }
K(FormatMessageA) {
    uint32_t buf = Arg(c, 4), size = Arg(c, 5);
    const char* msg = "error";
    if (Arg(c, 0) & 0x100) {  // FORMAT_MESSAGE_ALLOCATE_BUFFER
        W32(buf, GuestStrdup(msg));
    } else if (size) {
        strncpy(Str(buf), msg, size);
    }
    Return(c, uint32_t(strlen(msg)), 7);
}

// ---- threads ----
K(CreateThread) {
    uint32_t stack = Arg(c, 1), start = Arg(c, 2), param = Arg(c, 3), flags = Arg(c, 4), tid_out = Arg(c, 5);
    auto* t = new Thread();
    t->refs = 2;
    t->suspended = (flags & 4) != 0;  // CREATE_SUSPENDED
    uint32_t handle = NewHandle(t);
    std::atomic<uint32_t> id{0};
    std::thread([t, stack, start, param, &id]() {
        GuestThread* g = AttachThread(stack);
        t->host_tid = int(gettid());
        t->id = g->id;
        id.store(g->id);
        {
            std::unique_lock<std::mutex> lock(g_kernel_mutex);
            g_kernel_cv.wait(lock, [t] { return !t->suspended; });
        }
        uint32_t code = GuestCall(start, {param});
        std::lock_guard<std::mutex> lock(g_kernel_mutex);
        t->exit_code = code;
        t->done = true;
        t->Notify();
    }).detach();
    while (!id.load()) std::this_thread::yield();
    if (tid_out) W32(tid_out, id.load());
    Return(c, handle, 6);
}
K(ResumeThread) {
    auto* t = static_cast<Thread*>(HandleObject(Arg(c, 0)));
    uint32_t prev = 0;
    if (t && t->type == KType::Thread) {
        std::lock_guard<std::mutex> lock(g_kernel_mutex);
        prev = t->suspended ? 1 : 0;
        t->suspended = false;
        g_kernel_cv.notify_all();
    }
    Return(c, prev, 1);
}
K(SuspendThread) {
    Log("SuspendThread(%08x) ignored", Arg(c, 0));
    Return(c, 0, 1);
}
// Windows priorities -> nice values: the engine raises its audio mixer and render threads; on a phone they compete
// with half a dozen busy workers, so the hint matters.
// Threads the engine raises above normal (its render, streaming and audio feeder threads) are kept off the slowest
// cores (by cpu_capacity): left to the scheduler, and with power hints lowering clocks, Snapdragon phones park them
// on the little cores, where the audio feeder falls behind (choppy sound). Not with battery_saver.
static void AvoidLittleCores(int tid) {
#ifdef __ANDROID__
    static const cpu_set_t* fast = []() -> const cpu_set_t* {
        if (SettingInt("battery_saver", 0)) return nullptr;
        int caps[64], min_cap = -1, n = 0;
        for (; n < 64; ++n) {
            char path[96];
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", n);
            FILE* f = fopen(path, "r");
            if (!f) break;
            if (fscanf(f, "%d", &caps[n]) != 1) caps[n] = 0;
            fclose(f);
            if (min_cap < 0 || caps[n] < min_cap) min_cap = caps[n];
        }
        static cpu_set_t set;
        CPU_ZERO(&set);
        int count = 0;
        for (int i = 0; i < n; ++i) {
            if (caps[i] > min_cap) {
                CPU_SET(i, &set);
                ++count;
            }
        }
        return count ? &set : nullptr;
    }();
    if (fast) sched_setaffinity(pid_t(tid), sizeof(*fast), fast);
#else
    (void)tid;
#endif
}

K(SetThreadPriority) {
    uint32_t h = Arg(c, 0);
    int prio = int32_t(Arg(c, 1));
    int tid = 0;
    if (h == kCurrentThread) {
        tid = int(gettid());
    } else {
        std::lock_guard<std::mutex> lock(g_kernel_mutex);
        KObject* o = HandleObject(h);
        if (o && o->type == KType::Thread) tid = static_cast<Thread*>(o)->host_tid;
    }
    int nice = prio >= 15 ? -10 : prio <= -15 ? 10 : -2 * prio;  // TIME_CRITICAL / IDLE / -2..2
    if (tid) {
        int r = setpriority(PRIO_PROCESS, id_t(tid), nice);
        Log("SetThreadPriority(%d) -> nice %d on tid %d%s", prio, nice, tid, r == 0 ? "" : " (not permitted)");
        // THREAD_PRIORITY_HIGHEST: the engine's render thread; TIME_CRITICAL: its audio feeder
        if (prio == 2 || prio >= 15) PerfHintAddThread(tid);
        if (prio > 0) AvoidLittleCores(tid);
    }
    Return(c, 1, 2);
}
K(SetThreadAffinityMask) { Return(c, 1, 2); }
K(GetProcessAffinityMask) {
    W32(Arg(c, 1), 0xf);
    W32(Arg(c, 2), 0xf);
    Return(c, 1, 3);
}
K(GetExitCodeThread) {
    auto* t = static_cast<Thread*>(HandleObject(Arg(c, 0)));
    std::lock_guard<std::mutex> lock(g_kernel_mutex);
    W32(Arg(c, 1), t && t->type == KType::Thread ? t->exit_code : 0);
    Return(c, 1, 2);
}
K(TerminateThread) {
    Log("TerminateThread(%08x) ignored", Arg(c, 0));
    Return(c, 1, 2);
}
K(ExitThread) { Fatal("ExitThread(%u): not supported yet", Arg(c, 0)); }
// Sleep(0) yields the rest of the time slice. Spore's worker threads poll for work with it; on Windows that is
// nearly free, here each call is a sched_yield system call, and a thread that does it in a tight loop burns a core
// (a large part of the "libc" time on the phone). After 64 calls in a row (less than 2 ms apart) the thread sleeps
// 100 us instead, which costs a waiting worker nothing noticeable.
K(Sleep) {
    uint32_t ms = Arg(c, 0);
    if (ms == 0) {
        static thread_local uint32_t streak = 0;
        static thread_local std::chrono::steady_clock::time_point last;
        auto now = std::chrono::steady_clock::now();
        streak = now - last < std::chrono::milliseconds(2) ? streak + 1 : 0;
        last = now;
        if (streak > 64) std::this_thread::sleep_for(std::chrono::microseconds(100));
        else std::this_thread::yield();
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    Return(c, 0, 1);
}

// ---- fibers ----
K(ConvertThreadToFiber) { Return(c, FiberConvertThread(Arg(c, 0)), 1); }
K(CreateFiber) { Return(c, FiberCreate(Arg(c, 0), Arg(c, 1), Arg(c, 2)), 3); }
K(SwitchToFiber) {
    uint32_t target = Arg(c, 0);
    // Pop the call frame first: the resumed fiber continues from its own frame, and this one resumes here later.
    Return(c, 0, 1);
    FiberSwitch(target);
}
K(DeleteFiber) {
    FiberDelete(Arg(c, 0));
    Return(c, 0, 1);
}
K(ConvertFiberToThread) {
    FiberConvertToThread();
    Return(c, 1);
}

// ---- TLS ----
K(TlsAlloc) {
    std::lock_guard<std::mutex> lock(g_tls_mutex);
    for (int i = 0; i < 64; ++i) {
        if (!g_tls_used[i]) {
            g_tls_used[i] = true;
            Return(c, uint32_t(i));
            return;
        }
    }
    Return(c, 0xffffffffu);
}
K(TlsFree) {
    std::lock_guard<std::mutex> lock(g_tls_mutex);
    if (Arg(c, 0) < 64) g_tls_used[Arg(c, 0)] = false;
    Return(c, 1, 1);
}
K(TlsGetValue) {
    SetLastError(0);
    Return(c, R32(TebTlsSlot(int(Arg(c, 0)))), 1);
}
K(TlsSetValue) {
    W32(TebTlsSlot(int(Arg(c, 0))), Arg(c, 1));
    Return(c, 1, 2);
}

// ---- synchronization ----
K(InitializeCriticalSection) {
    CriticalSection(Arg(c, 0));
    Return(c, 0, 1);
}
K(DeleteCriticalSection) { Return(c, 0, 1); }
K(EnterCriticalSection) {
    CriticalSection(Arg(c, 0))->lock();
    Return(c, 0, 1);
}
K(LeaveCriticalSection) {
    CriticalSection(Arg(c, 0))->unlock();
    Return(c, 0, 1);
}
K(TryEnterCriticalSection) { Return(c, CriticalSection(Arg(c, 0))->try_lock() ? 1 : 0, 1); }

K(CreateEventA) { Return(c, NewHandle(new Event(Arg(c, 1) != 0, Arg(c, 2) != 0)), 4); }
K(SetEvent) {
    auto* e = static_cast<Event*>(HandleObject(Arg(c, 0)));
    std::lock_guard<std::mutex> lock(g_kernel_mutex);
    if (e && e->type == KType::Event) {
        e->state = true;
        e->Notify();
    }
    Return(c, e ? 1 : 0, 1);
}
K(ResetEvent) {
    auto* e = static_cast<Event*>(HandleObject(Arg(c, 0)));
    std::lock_guard<std::mutex> lock(g_kernel_mutex);
    if (e && e->type == KType::Event) e->state = false;
    Return(c, e ? 1 : 0, 1);
}
K(CreateSemaphoreA) { Return(c, NewHandle(new Semaphore(int32_t(Arg(c, 1)), int32_t(Arg(c, 2)))), 4); }
K(ReleaseSemaphore) {
    auto* s = static_cast<Semaphore*>(HandleObject(Arg(c, 0)));
    std::lock_guard<std::mutex> lock(g_kernel_mutex);
    if (!s || s->type != KType::Semaphore) {
        Return(c, 0, 3);
        return;
    }
    if (Arg(c, 2)) W32(Arg(c, 2), uint32_t(s->count));
    s->count += int32_t(Arg(c, 1));
    s->Notify();
    Return(c, 1, 3);
}
K(CreateMutexA) {
    auto* m = new Mutex();
    if (Arg(c, 1)) m->Acquire(CurrentTid());
    Return(c, NewHandle(m), 3);
}
K(WaitForSingleObject) {
    KObject* o = HandleObject(Arg(c, 0));
    if (!o) {
        SetLastError(ERROR_INVALID_HANDLE);
        Return(c, 0xffffffffu, 2);
        return;
    }
    uint32_t tid = CurrentTid();
    std::unique_lock<std::mutex> lock(g_kernel_mutex);
    bool ok = WaitKernel(lock, Arg(c, 1), &o, 1, [&] { return o->Signaled(tid); });
    if (ok) o->Acquire(tid);
    Return(c, ok ? 0 : 0x102, 2);  // WAIT_OBJECT_0 / WAIT_TIMEOUT
}
uint32_t WaitMultiple(uint32_t n, uint32_t handles, bool all, uint32_t ms) {
    std::vector<KObject*> objs;
    for (uint32_t i = 0; i < n; ++i) objs.push_back(HandleObject(R32(handles + 4 * i)));
    uint32_t tid = CurrentTid();
    uint32_t which = 0x102;
    std::unique_lock<std::mutex> lock(g_kernel_mutex);
    WaitKernel(lock, ms, objs.data(), objs.size(), [&] {
        if (all) {
            for (KObject* o : objs)
                if (!o || !o->Signaled(tid)) return false;
            for (KObject* o : objs) o->Acquire(tid);
            which = 0;
            return true;
        }
        for (uint32_t i = 0; i < objs.size(); ++i) {
            if (objs[i] && objs[i]->Signaled(tid)) {
                objs[i]->Acquire(tid);
                which = i;
                return true;
            }
        }
        return false;
    });
    return which;
}
K(WaitForMultipleObjects) { Return(c, WaitMultiple(Arg(c, 0), Arg(c, 1), Arg(c, 2) != 0, Arg(c, 3)), 4); }

// ---- APCs (QueueUserAPC): run by the target thread in its next alertable wait (SleepEx, WaitFor*Ex), which
// waits in short slices so that a newly queued APC is noticed ----
std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> g_apcs;  // guest thread id -> (fn, data); g_kernel_mutex
bool RunApcs() {
    std::vector<std::pair<uint32_t, uint32_t>> todo;
    {
        std::lock_guard<std::mutex> lock(g_kernel_mutex);
        auto it = g_apcs.find(CurrentTid());
        if (it == g_apcs.end() || it->second.empty()) return false;
        todo.swap(it->second);
    }
    for (auto& [fn, data] : todo) GuestCall(fn, {data});
    return true;
}
constexpr uint32_t kWaitIoCompletion = 0xc0;
uint32_t AlertableWait(uint32_t ms, const std::function<uint32_t(uint32_t)>& wait_slice) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        if (RunApcs()) return kWaitIoCompletion;
        uint32_t slice = 10;
        if (ms != 0xffffffffu) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
            if (left.count() <= 0) slice = 0;
            else if (left.count() < 10) slice = uint32_t(left.count());
        }
        uint32_t r = wait_slice(slice);
        if (r != 0x102 || (ms != 0xffffffffu && std::chrono::steady_clock::now() >= end)) return r;
    }
}

K(CloseHandle) {
    CloseKernelHandle(Arg(c, 0));
    Return(c, 1, 1);
}

// ---- files ----
K(CreateFileA) {
    const char* name = Str(Arg(c, 0));
    uint32_t access = Arg(c, 1), disposition = Arg(c, 4);
    std::string host = HostPath(name);
    int flags = 0;
    bool write = (access & 0x40000000u) || (access & 0x10000000u);  // GENERIC_WRITE / GENERIC_ALL
    bool read = (access & 0x80000000u) || (access & 0x10000000u) || !write;
    flags = read && write ? O_RDWR : write ? O_WRONLY : O_RDONLY;
    switch (disposition) {
        case 1: flags |= O_CREAT | O_EXCL; break;   // CREATE_NEW
        case 2: flags |= O_CREAT | O_TRUNC; break;  // CREATE_ALWAYS
        case 3: break;                              // OPEN_EXISTING
        case 4: flags |= O_CREAT; break;            // OPEN_ALWAYS
        case 5: flags |= O_TRUNC; break;            // TRUNCATE_EXISTING
    }
    struct stat st;
    bool existed = stat(host.c_str(), &st) == 0;
    if (existed && S_ISDIR(st.st_mode)) {
        SetLastError(ERROR_ACCESS_DENIED);
        Return(c, kInvalidHandle, 7);
        return;
    }
    if (flags & O_CREAT) {
        // Create missing parent directories under native_c (saves) lazily.
        std::string dir = host.substr(0, host.find_last_of('/'));
        mkdir(dir.c_str(), 0755);
    }
    int fd = open(host.c_str(), flags | O_CLOEXEC, 0644);
    if (fd < 0 && (errno == EACCES || errno == EPERM) && write && existed) {
        // Files put into the app's folder by hand (a file manager, a PC, adb) belong to another user: the app can
        // read them but not write them, and the game opens its profile files (achievements, saves) for writing. A
        // failed open there left the game with a database without a file, and it crashed on start. The file is
        // copied into a new one the app owns; if that is not possible, it is opened read-only.
        std::vector<char> data;
        int in = open(host.c_str(), O_RDONLY | O_CLOEXEC);
        if (in >= 0) {
            char buf[65536];
            for (ssize_t n; (n = ::read(in, buf, sizeof buf)) > 0;) data.insert(data.end(), buf, buf + n);
            close(in);
            std::string tmp = host + ".sporere-copy";
            int out = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            bool copied = out >= 0 && (data.empty() || ::write(out, data.data(), data.size()) == ssize_t(data.size()));
            if (out >= 0) close(out);
            if (copied && rename(tmp.c_str(), host.c_str()) == 0) {
                fd = open(host.c_str(), flags | O_CLOEXEC, 0644);
                Log("CreateFileA(%s): no write access, made a copy the app owns -> %s", name, fd < 0 ? strerror(errno) : "ok");
            } else {
                unlink(tmp.c_str());
                fd = open(host.c_str(), (flags & ~(O_ACCMODE | O_TRUNC | O_CREAT)) | O_RDONLY | O_CLOEXEC);
                Log("CreateFileA(%s): no write access, opened read-only -> %s", name, fd < 0 ? strerror(errno) : "ok");
            }
        }
    }
    const int open_errno = errno;
    // a missing file is routine (the game probes for many); anything else is worth a line in the log
    if (fd < 0 && open_errno != ENOENT && open_errno != ENOTDIR)
        Log("CreateFileA(%s, access %x, disp %u) failed: %s", name, access, disposition, strerror(open_errno));
    static const bool trace = getenv("SPORERE_TRACE_FILES") != nullptr;
    if (trace) Log("CreateFileA(%s, %x, disp %u) -> %s", name, access, disposition, fd < 0 ? strerror(open_errno) : "ok");
    if (fd < 0) {
        SetLastError(ErrnoToWin(open_errno));
        Return(c, kInvalidHandle, 7);
        return;
    }
    auto* f = new File();
    f->fd = fd;
    static const bool read_cache = SettingInt("read_cache", 1) != 0;
    f->cached = read_cache && (flags & O_ACCMODE) == O_RDONLY;
    SetLastError((disposition == 2 || disposition == 4) && existed ? ERROR_ALREADY_EXISTS : 0);
    Return(c, NewHandle(f), 7);
}
File* FileOf(uint32_t h) {
    KObject* o = HandleObject(h);
    return o && o->type == KType::File ? static_cast<File*>(o) : nullptr;
}
// OVERLAPPED: [8] Offset, [12] OffsetHigh, [16] hEvent. I/O completes synchronously.
void CompleteOverlapped(uint32_t ov, uint32_t bytes) {
    W32(ov, 0);
    W32(ov + 4, bytes);
    if (uint32_t ev = R32(ov + 16)) {
        auto* e = static_cast<Event*>(HandleObject(ev & ~1u));
        std::lock_guard<std::mutex> lock(g_kernel_mutex);
        if (e && e->type == KType::Event) {
            e->state = true;
            e->Notify();
        }
    }
}
K(ReadFile) {
    File* f = FileOf(Arg(c, 0));
    uint32_t buf = Arg(c, 1), n = Arg(c, 2), out = Arg(c, 3), ov = Arg(c, 4);
    if (!f) {
        SetLastError(ERROR_INVALID_HANDLE);
        Return(c, 0, 5);
        return;
    }
    auto t0 = std::chrono::steady_clock::now();
    ssize_t r;
    if (f->cached) {
        std::lock_guard<std::mutex> lock(f->mu);
        int64_t off = ov ? int64_t(uint64_t(R32(ov + 8)) | (uint64_t(R32(ov + 12)) << 32)) : f->pos;
        r = f->CachedRead(Ptr<uint8_t>(buf), n, off);
        if (!ov && r > 0) f->pos = off + r;
    } else {
        r = ov ? pread(f->fd, Ptr<void>(buf), n, off_t(R32(ov + 8)) | (off_t(R32(ov + 12)) << 32))
               : read(f->fd, Ptr<void>(buf), n);
    }
    uint64_t us = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    g_io.reads.fetch_add(1, std::memory_order_relaxed);
    g_io.total_us.fetch_add(us, std::memory_order_relaxed);
    g_io.by_size[n < 4096 ? 0 : n < 65536 ? 1 : n < (1u << 20) ? 2 : 3].fetch_add(1, std::memory_order_relaxed);
    if (r > 0) g_io.bytes.fetch_add(uint64_t(r), std::memory_order_relaxed);
    if (us > 50000) g_io.slow.fetch_add(1, std::memory_order_relaxed);
    for (uint64_t m = g_io.max_us.load(); us > m && !g_io.max_us.compare_exchange_weak(m, us);) {
    }
    if (r < 0) {
        SetLastError(ErrnoToWin(errno));
        Return(c, 0, 5);
        return;
    }
    if (out) W32(out, uint32_t(r));
    if (ov) CompleteOverlapped(ov, uint32_t(r));
    Return(c, 1, 5);
}
K(WriteFile) {
    File* f = FileOf(Arg(c, 0));
    uint32_t buf = Arg(c, 1), n = Arg(c, 2), out = Arg(c, 3), ov = Arg(c, 4);
    if (!f) {
        SetLastError(ERROR_INVALID_HANDLE);
        Return(c, 0, 5);
        return;
    }
    ssize_t r = ov ? pwrite(f->fd, Ptr<void>(buf), n, off_t(R32(ov + 8)) | (off_t(R32(ov + 12)) << 32))
                   : write(f->fd, Ptr<void>(buf), n);
    if (r < 0) {
        SetLastError(ErrnoToWin(errno));
        Return(c, 0, 5);
        return;
    }
    if (out) W32(out, uint32_t(r));
    if (ov) CompleteOverlapped(ov, uint32_t(r));
    Return(c, 1, 5);
}
K(SetFilePointer) {
    File* f = FileOf(Arg(c, 0));
    int64_t dist = int32_t(Arg(c, 1));
    uint32_t high = Arg(c, 2);
    if (high) dist = int64_t((uint64_t(R32(high)) << 32) | Arg(c, 1));
    off_t r = -1;
    if (f && f->cached) {
        std::lock_guard<std::mutex> lock(f->mu);
        int whence = int(Arg(c, 3));  // FILE_BEGIN/CURRENT/END = SEEK_*
        struct stat st;
        int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? f->pos
                       : fstat(f->fd, &st) == 0 ? int64_t(st.st_size) : -1;
        if (base >= 0 && base + dist >= 0) r = off_t(f->pos = base + dist);
    } else if (f) {
        r = lseek(f->fd, off_t(dist), int(Arg(c, 3)));
    }
    if (r < 0) {
        SetLastError(f ? 131 : ERROR_INVALID_HANDLE);  // ERROR_NEGATIVE_SEEK
        Return(c, 0xffffffffu, 4);
        return;
    }
    if (high) W32(high, uint32_t(uint64_t(r) >> 32));
    SetLastError(0);
    Return(c, uint32_t(r), 4);
}
K(GetFileSize) {
    File* f = FileOf(Arg(c, 0));
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) {
        Return(c, 0xffffffffu, 2);
        return;
    }
    if (Arg(c, 1)) W32(Arg(c, 1), uint32_t(uint64_t(st.st_size) >> 32));
    Return(c, uint32_t(st.st_size), 2);
}
K(SetEndOfFile) {
    File* f = FileOf(Arg(c, 0));
    Return(c, f && ftruncate(f->fd, lseek(f->fd, 0, SEEK_CUR)) == 0 ? 1 : 0, 1);
}
K(FlushFileBuffers) { Return(c, 1, 1); }
K(GetFileTime) {
    File* f = FileOf(Arg(c, 0));
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) {
        Return(c, 0, 4);
        return;
    }
    uint64_t t = ToFileTime(st.st_mtim);
    for (int i = 1; i <= 3; ++i)
        if (Arg(c, i)) W64(Arg(c, i), t);
    Return(c, 1, 4);
}
K(CompareFileTime) {
    uint64_t a = R64(Arg(c, 0)), b = R64(Arg(c, 1));
    Return(c, a < b ? 0xffffffffu : a > b ? 1 : 0, 2);
}
K(DeleteFileA) {
    int r = unlink(HostPath(Str(Arg(c, 0))).c_str());
    if (r) SetLastError(ErrnoToWin(errno));
    Return(c, r == 0, 1);
}
K(MoveFileA) {
    int r = rename(HostPath(Str(Arg(c, 0))).c_str(), HostPath(Str(Arg(c, 1))).c_str());
    if (r) SetLastError(ErrnoToWin(errno));
    Return(c, r == 0, 2);
}
K(CopyFileA) {
    std::string from = HostPath(Str(Arg(c, 0))), to = HostPath(Str(Arg(c, 1)));
    struct stat st;
    if (Arg(c, 2) && stat(to.c_str(), &st) == 0) {
        SetLastError(ERROR_FILE_EXISTS);
        Return(c, 0, 3);
        return;
    }
    FILE* in = fopen(from.c_str(), "rb");
    FILE* out = in ? fopen(to.c_str(), "wb") : nullptr;
    bool ok = in && out;
    char buf[65536];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    if (in) fclose(in);
    if (out) fclose(out);
    if (!ok) SetLastError(ERROR_FILE_NOT_FOUND);
    Return(c, ok, 3);
}
K(GetFileAttributesA) {
    struct stat st;
    if (stat(HostPath(Str(Arg(c, 0))).c_str(), &st) != 0) {
        SetLastError(ErrnoToWin(errno));
        Return(c, 0xffffffffu, 1);
        return;
    }
    Return(c, Attributes(st), 1);
}
K(SetFileAttributesA) { Return(c, 1, 2); }
K(CreateDirectoryA) {
    std::string host = HostPath(Str(Arg(c, 0)));
    int r = mkdir(host.c_str(), 0755);
    if (r) SetLastError(errno == EEXIST ? ERROR_ALREADY_EXISTS : ErrnoToWin(errno));
    Return(c, r == 0, 2);
}
K(RemoveDirectoryA) { Return(c, rmdir(HostPath(Str(Arg(c, 0))).c_str()) == 0, 1); }
K(FindFirstFileA) {
    std::string full = FullGuestPath(Str(Arg(c, 0)));
    size_t slash = full.find_last_of('\\');
    auto* f = new Find();
    f->dir = HostPath(full.substr(0, slash).c_str());
    f->pattern = full.substr(slash + 1);
    if (f->pattern == "*.*") f->pattern = "*";
    if (DIR* d = opendir(f->dir.c_str())) {
        while (dirent* e = readdir(d)) f->names.push_back(e->d_name);
        closedir(d);
    }
    if (!FindNextMatch(f, Arg(c, 1))) {
        delete f;
        SetLastError(ERROR_FILE_NOT_FOUND);
        Return(c, kInvalidHandle, 2);
        return;
    }
    Return(c, NewHandle(f), 2);
}
K(FindNextFileA) {
    KObject* o = HandleObject(Arg(c, 0));
    if (!o || o->type != KType::Find || !FindNextMatch(static_cast<Find*>(o), Arg(c, 1))) {
        SetLastError(ERROR_NO_MORE_FILES);
        Return(c, 0, 2);
        return;
    }
    Return(c, 1, 2);
}
K(FindClose) {
    CloseKernelHandle(Arg(c, 0));
    Return(c, 1, 1);
}
K(GetFullPathNameA) {
    std::string full = FullGuestPath(Str(Arg(c, 0)));
    uint32_t size = Arg(c, 1), buf = Arg(c, 2), part = Arg(c, 3);
    if (full.size() + 1 > size) {
        Return(c, uint32_t(full.size() + 1), 4);
        return;
    }
    strcpy(Str(buf), full.c_str());
    if (part) W32(part, buf + uint32_t(full.find_last_of('\\') + 1));
    Return(c, uint32_t(full.size()), 4);
}
K(GetCurrentDirectoryA) {
    std::string d = GuestCurrentDir();
    uint32_t size = Arg(c, 0), buf = Arg(c, 1);
    if (d.size() + 1 > size) {
        Return(c, uint32_t(d.size() + 1), 2);
        return;
    }
    strcpy(Str(buf), d.c_str());
    Return(c, uint32_t(d.size()), 2);
}
K(SetCurrentDirectoryA) {
    SetGuestCurrentDir(FullGuestPath(Str(Arg(c, 0))));
    Return(c, 1, 1);
}
K(GetDriveTypeA) { Return(c, 3, 1); }  // DRIVE_FIXED
K(GetLogicalDrives) { Return(c, 1u << 2); }
K(GetVolumeInformationA) {
    if (Arg(c, 1)) strncpy(Str(Arg(c, 1)), "PROTO", Arg(c, 2));
    if (Arg(c, 3)) W32(Arg(c, 3), 0x12345678u);
    if (Arg(c, 4)) W32(Arg(c, 4), 255);
    if (Arg(c, 5)) W32(Arg(c, 5), 0x3);
    if (Arg(c, 6)) strncpy(Str(Arg(c, 6)), "NTFS", Arg(c, 7));
    Return(c, 1, 8);
}
K(GetDiskFreeSpaceExA) {
    struct statvfs s;
    uint64_t free_bytes = statvfs(g_game_dir, &s) == 0 ? uint64_t(s.f_bavail) * s.f_frsize : (1ull << 34);
    for (int i = 1; i <= 3; ++i)
        if (Arg(c, i)) W64(Arg(c, i), free_bytes);
    Return(c, 1, 4);
}
K(DeviceIoControl) {
    SetLastError(1);  // ERROR_INVALID_FUNCTION
    Return(c, 0, 8);
}
K(OpenFile) {
    // OFSTRUCT-based open; the engine only probes for existence (OF_EXIST).
    struct stat st;
    bool exists = stat(HostPath(Str(Arg(c, 0))).c_str(), &st) == 0;
    Return(c, exists ? 1 : 0xffffffffu, 3);
}

// ---- locale / strings ----
K(MultiByteToWideChar) {
    uint32_t src = Arg(c, 2), out = Arg(c, 4), out_size = Arg(c, 5);
    int32_t n = int32_t(Arg(c, 3));
    if (n < 0) n = int32_t(strlen(Str(src))) + 1;
    if (out_size == 0) {
        Return(c, uint32_t(n), 6);
        return;
    }
    int32_t m = n < int32_t(out_size) ? n : int32_t(out_size);
    for (int32_t i = 0; i < m; ++i) W16(out + 2 * i, R8(src + i));  // Latin-1 code page subset
    Return(c, uint32_t(m), 6);
}
K(lstrlenW) {
    uint32_t s = Arg(c, 0), n = 0;
    if (s)
        while (R16(s + 2 * n)) ++n;
    Return(c, n, 1);
}
K(GetACP) { Return(c, 1252); }
K(GetThreadLocale) { Return(c, 1033); }
K(GetSystemDefaultLangID) { Return(c, 1033); }
K(GetLocaleInfoA) {
    uint32_t type = Arg(c, 1), buf = Arg(c, 2), size = Arg(c, 3);
    const char* v = "";
    switch (type & 0xffff) {
        case 0x0003: v = "ENU"; break;            // LOCALE_SABBREVLANGNAME
        case 0x0059: v = "en"; break;             // LOCALE_SISO639LANGNAME
        case 0x005a: v = "US"; break;             // LOCALE_SISO3166CTRYNAME
        case 0x000e: v = "."; break;              // LOCALE_SDECIMAL
        case 0x000f: v = ","; break;              // LOCALE_STHOUSAND
        case 0x1004: v = "1252"; break;           // LOCALE_IDEFAULTANSICODEPAGE
        default: v = "";
    }
    uint32_t n = uint32_t(strlen(v)) + 1;
    if (size) strncpy(Str(buf), v, size);
    Return(c, n, 4);
}


#define E(name) {"kernel32.dll", #name, k_##name}

// ---- SporeApp.exe: the wide (UTF-16) variants and the rest of what its CRT and engine import ----
// Wide strings are converted to UTF-8 (the host's file names) and handed to the ANSI implementations above, which
// treat them as opaque bytes; results go back as UTF-16.
std::string Wide(uint32_t a, int32_t n = -1) {
    std::string out;
    if (!a) return out;
    for (int32_t i = 0; n < 0 || i < n; ++i) {
        uint32_t ch = R16(a + 2 * uint32_t(i));
        if (n < 0 && !ch) break;
        if (ch >= 0xd800 && ch < 0xdc00 && (n < 0 || i + 1 < n)) {
            uint32_t lo = R16(a + 2 * uint32_t(i + 1));
            if (lo >= 0xdc00 && lo < 0xe000) {
                ch = 0x10000 + ((ch - 0xd800) << 10) + (lo - 0xdc00);
                ++i;
            }
        }
        if (ch < 0x80) {
            out += char(ch);
        } else if (ch < 0x800) {
            out += char(0xc0 | ch >> 6);
            out += char(0x80 | (ch & 0x3f));
        } else if (ch < 0x10000) {
            out += char(0xe0 | ch >> 12);
            out += char(0x80 | ((ch >> 6) & 0x3f));
            out += char(0x80 | (ch & 0x3f));
        } else {
            out += char(0xf0 | ch >> 18);
            out += char(0x80 | ((ch >> 12) & 0x3f));
            out += char(0x80 | ((ch >> 6) & 0x3f));
            out += char(0x80 | (ch & 0x3f));
        }
    }
    return out;
}
std::u16string ToUtf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = uint8_t(s[i++]);
        int extra = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
        cp &= extra == 3 ? 0x07 : extra == 2 ? 0x0f : extra == 1 ? 0x1f : 0x7f;
        for (; extra && i < s.size(); --extra) cp = cp << 6 | (uint8_t(s[i++]) & 0x3f);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += char16_t(0xd800 + (cp >> 10));
            out += char16_t(0xdc00 + (cp & 0x3ff));
        } else {
            out += char16_t(cp);
        }
    }
    return out;
}
// Writes s (with its terminator) into a guest buffer of `cap` UTF-16 units; returns the length without the
// terminator, or the size needed (with it) when it does not fit.
uint32_t PutWide(uint32_t buf, uint32_t cap, const std::string& s, bool* fits = nullptr) {
    std::u16string w = ToUtf16(s);
    bool ok = buf && cap > w.size();
    if (fits) *fits = ok;
    if (!ok) return uint32_t(w.size() + 1);
    for (size_t i = 0; i < w.size(); ++i) W16(buf + 2 * uint32_t(i), uint16_t(w[i]));
    W16(buf + 2 * uint32_t(w.size()), 0);
    return uint32_t(w.size());
}

// Calls an ANSI handler with the wide string arguments at `which` replaced by UTF-8 copies; `extra` dwords of the
// wide variant's arguments that the ANSI one does not have are popped afterwards.
void CallAnsi(Ctx* c, void (*ansi)(Ctx*), std::initializer_list<int> which, int extra = 0) {
    std::vector<std::pair<int, uint32_t>> saved;
    for (int i : which) {
        uint32_t w = Arg(c, i);
        if (!w) continue;
        uint32_t a = GuestStrdup(Wide(w).c_str());
        saved.push_back({i, w});
        W32(c->esp + 4 + 4 * uint32_t(i), a);
    }
    std::vector<uint32_t> temps;
    for (auto& [i, w] : saved) temps.push_back(Arg(c, i));
    uint32_t esp = c->esp;
    ansi(c);
    // The arguments are still in place below the popped return address: restore them for the caller's view.
    for (size_t k = 0; k < saved.size(); ++k) {
        W32(esp + 4 + 4 * uint32_t(saved[k].first), saved[k].second);
        HeapFree(temps[k]);
    }
    c->esp += 4 * uint32_t(extra);
}

K(GetStartupInfoA) {
    uint32_t si = Arg(c, 0);
    memset(Ptr<void>(si), 0, 68);
    W32(si, 68);
    Return(c, 0, 1);
}
K(GetCommandLineA) {
    static uint32_t line = GuestStrdup("\"C:\\Game\\SporebinEP1\\SporeApp.exe\"");
    Return(c, line);
}
K(GetStdHandle) { Return(c, 0x00000f00u - Arg(c, 0), 1); }  // -10/-11/-12 -> distinct fake handles
K(GetFileType) {
    uint32_t h = Arg(c, 0);
    Return(c, (h >= 0xf0a && h <= 0xf0c) ? 2u : 1u, 1);  // FILE_TYPE_CHAR for the std handles, DISK otherwise
}
K(FlushConsoleInputBuffer) { Return(c, 1, 1); }
K(GetVersion) { Return(c, 7601u << 16 | 1u << 8 | 6u); }  // Windows 7 SP1
K(GetVersionExW) {
    uint32_t v = Arg(c, 0), size = R32(v);
    memset(Ptr<uint8_t>(v) + 4, 0, size > 4 ? size - 4 : 0);
    W32(v + 4, 6);
    W32(v + 8, 1);
    W32(v + 12, 7601);
    W32(v + 16, 2);  // VER_PLATFORM_WIN32_NT
    PutWide(v + 20, 128, "Service Pack 1");
    if (size >= 284) W16(v + 276, 1);
    Return(c, 1, 1);
}
K(SetLastError) {
    SetLastError(Arg(c, 0));
    Return(c, 0, 1);
}
K(InterlockedIncrement) { Return(c, __atomic_add_fetch(Ptr<uint32_t>(Arg(c, 0)), 1u, __ATOMIC_SEQ_CST), 1); }
K(InterlockedExchangeAdd) {
    Return(c, __atomic_fetch_add(Ptr<uint32_t>(Arg(c, 0)), Arg(c, 1), __ATOMIC_SEQ_CST), 2);
}
uint32_t ZeroedAlloc(uint32_t size, bool zero) {
    uint32_t p = HeapAlloc(size ? size : 1);
    if (p && zero) memset(Ptr<void>(p), 0, size);
    return p;
}
K(HeapAlloc) { Return(c, ZeroedAlloc(Arg(c, 2), Arg(c, 1) & 0x8u), 3); }  // HEAP_ZERO_MEMORY
K(LocalAlloc) { Return(c, ZeroedAlloc(Arg(c, 1), Arg(c, 0) & 0x40u), 2); }  // LMEM_ZEROINIT
K(LocalFree) {
    if (Arg(c, 0)) HeapFree(Arg(c, 0));
    Return(c, 0, 1);
}
K(GlobalAlloc) { Return(c, ZeroedAlloc(Arg(c, 1), Arg(c, 0) & 0x40u), 2); }  // GMEM_ZEROINIT; never moveable
K(GlobalLock) { Return(c, Arg(c, 0), 1); }
K(GlobalUnlock) { Return(c, 1, 1); }
K(GlobalFree) {
    if (Arg(c, 0)) HeapFree(Arg(c, 0));
    Return(c, 0, 1);
}
K(GlobalSize) { Return(c, Arg(c, 0) ? HeapSize(Arg(c, 0)) : 0, 1); }
K(GlobalMemoryStatusEx) {
    uint32_t m = Arg(c, 0);
    uint64_t phys = uint64_t(sysconf(_SC_PHYS_PAGES)) * uint64_t(sysconf(_SC_PAGESIZE));
    uint64_t avail = uint64_t(sysconf(_SC_AVPHYS_PAGES)) * uint64_t(sysconf(_SC_PAGESIZE));
    W32(m + 4, phys ? uint32_t(100 - avail * 100 / phys) : 0);
    auto w64 = [&](uint32_t off, uint64_t v) { W32(m + off, uint32_t(v)); W32(m + off + 4, uint32_t(v >> 32)); };
    w64(8, phys);
    w64(16, avail);
    w64(24, phys);
    w64(32, avail);
    w64(40, 0x7ffe0000ull);  // 2 GiB user space: not large-address aware
    w64(48, 0x60000000ull);
    w64(56, 0);
    Return(c, 1, 1);
}
K(GetSystemInfo) {
    uint32_t si = Arg(c, 0);
    memset(Ptr<void>(si), 0, 36);
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0 || n > 32) n = n ? 32 : 4;
    W32(si + 4, 4096);
    W32(si + 8, 0x00010000u);
    W32(si + 12, 0x7ffeffffu);
    W32(si + 16, n == 32 ? 0xffffffffu : (1u << n) - 1);
    W32(si + 20, n);
    W32(si + 24, 586);  // PROCESSOR_INTEL_PENTIUM
    W32(si + 28, 0x10000);
    W16(si + 32, 6);
    Return(c, 0, 1);
}
K(SetPriorityClass) { Return(c, 1, 2); }
K(GetPriorityClass) { Return(c, 0x20, 1); }  // NORMAL_PRIORITY_CLASS
K(GetThreadPriority) { Return(c, 0, 1); }
K(SetProcessAffinityMask) { Return(c, 1, 2); }
K(SetThreadIdealProcessor) { Return(c, 0, 2); }
K(SetThreadExecutionState) { Return(c, 0x80000000u, 1); }
void PutComputerName(uint32_t buf, uint32_t size_ptr) {
    const char* name = "SPORE";
    uint32_t cap = R32(size_ptr);
    if (cap > strlen(name)) strcpy(Str(buf), name);
    W32(size_ptr, uint32_t(strlen(name)));
}
K(GetComputerNameA) {
    PutComputerName(Arg(c, 0), Arg(c, 1));
    Return(c, 1, 2);
}
K(GetComputerNameExA) {
    PutComputerName(Arg(c, 1), Arg(c, 2));
    Return(c, 1, 3);
}
K(OpenProcess) { Return(c, kCurrentProcess, 3); }  // the game only ever opens itself
K(GetProcessTimes) {
    uint64_t now = NowFileTime();
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    uint64_t user = uint64_t(ts.tv_sec) * 10000000u + uint64_t(ts.tv_nsec) / 100u;
    uint64_t vals[4] = {now, 0, 0, user};
    for (int i = 0; i < 4; ++i) {
        if (!Arg(c, 1 + i)) continue;
        W32(Arg(c, 1 + i), uint32_t(vals[i]));
        W32(Arg(c, 1 + i) + 4, uint32_t(vals[i] >> 32));
    }
    Return(c, 1, 5);
}
K(DuplicateHandle) {
    // Pseudo handles (GetCurrentThread/Process) stay what they are; real ones are shared, not copied.
    uint32_t src = Arg(c, 1), dst = Arg(c, 3);
    if (KObject* o = HandleObject(src)) {
        std::lock_guard<std::mutex> lock(g_kernel_mutex);
        ++o->refs;
    }
    if (dst) W32(dst, src);
    Return(c, 1, 7);
}
K(SleepEx) {
    uint32_t ms = Arg(c, 0);
    if (!Arg(c, 1)) {
        CallAnsi(c, k_Sleep, {}, 1);
        c->eax = 0;
        return;
    }
    uint32_t r = AlertableWait(ms, [](uint32_t slice) {
        std::this_thread::sleep_for(std::chrono::milliseconds(slice ? slice : 1));
        return 0x102u;
    });
    Return(c, r == kWaitIoCompletion ? r : 0, 2);
}
K(QueueUserAPC) {
    uint32_t fn = Arg(c, 0), h = Arg(c, 1), data = Arg(c, 2);
    uint32_t tid = 0;
    if (h == kCurrentThread) tid = CurrentTid();
    else if (KObject* o = HandleObject(h); o && o->type == KType::Thread) tid = static_cast<Thread*>(o)->id;
    if (!tid) {
        Return(c, 0, 3);
        return;
    }
    std::lock_guard<std::mutex> lock(g_kernel_mutex);
    g_apcs[tid].push_back({fn, data});
    Return(c, 1, 3);
}
K(WaitForMultipleObjectsEx) {
    uint32_t n = Arg(c, 0), handles = Arg(c, 1), ms = Arg(c, 3);
    bool all = Arg(c, 2) != 0;
    if (!Arg(c, 4)) {
        Return(c, WaitMultiple(n, handles, all, ms), 5);
        return;
    }
    Return(c, AlertableWait(ms, [&](uint32_t slice) { return WaitMultiple(n, handles, all, slice); }), 5);
}
K(ReleaseMutex) {
    KObject* o = HandleObject(Arg(c, 0));
    if (!o || o->type != KType::Mutex) {
        Return(c, 0, 1);
        return;
    }
    auto* m = static_cast<Mutex*>(o);
    std::lock_guard<std::mutex> lock(g_kernel_mutex);
    if (m->owner != CurrentTid()) {
        Return(c, 0, 1);
        return;
    }
    if (--m->recursion == 0) {
        m->owner = 0;
        m->Notify();
        g_kernel_cv.notify_all();
    }
    Return(c, 1, 1);
}
K(InitializeCriticalSectionAndSpinCount) {
    CallAnsi(c, k_InitializeCriticalSection, {}, 1);
    c->eax = 1;
}
K(VirtualQuery) {
    uint32_t addr = Arg(c, 0), mbi = Arg(c, 1);
    uint32_t page = addr & ~0xfffu;
    GuestThread* t = CurrentThread();
    uint32_t alloc = page, region = 0x1000, protect = 0x04;  // PAGE_READWRITE
    if (t && addr >= t->stack_lo && addr < t->stack_hi) {
        alloc = t->stack_lo;  // the CRT and engines find their stack size from AllocationBase
        region = t->stack_hi - page;
    } else if (addr >= g_engine.base && addr < g_engine.base + g_engine.size) {
        alloc = g_engine.base;
        protect = 0x20;  // PAGE_EXECUTE_READ
        region = g_engine.base + g_engine.size - page;
    }
    W32(mbi, page);
    W32(mbi + 4, alloc);
    W32(mbi + 8, protect);
    W32(mbi + 12, region);
    W32(mbi + 16, 0x1000);  // MEM_COMMIT
    W32(mbi + 20, protect);
    W32(mbi + 24, alloc == g_engine.base ? 0x1000000u : 0x20000u);  // MEM_IMAGE / MEM_PRIVATE
    Return(c, 28, 3);
}
K(GetSystemTime) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm t;
    gmtime_r(&tv.tv_sec, &t);
    WriteSystemTime(Arg(c, 0), t, int(tv.tv_usec / 1000));
    Return(c, 0, 1);
}
K(SystemTimeToFileTime) {
    uint32_t st = Arg(c, 0);
    struct tm t {};
    t.tm_year = int(R16(st)) - 1900;
    t.tm_mon = int(R16(st + 2)) - 1;
    t.tm_mday = int(R16(st + 6));
    t.tm_hour = int(R16(st + 8));
    t.tm_min = int(R16(st + 10));
    t.tm_sec = int(R16(st + 12));
    int64_t secs = int64_t(timegm(&t));
    uint64_t ft = uint64_t(secs + 11644473600ll) * 10000000u + uint64_t(R16(st + 14)) * 10000u;
    W32(Arg(c, 1), uint32_t(ft));
    W32(Arg(c, 1) + 4, uint32_t(ft >> 32));
    Return(c, 1, 2);
}
K(GetSystemDefaultLCID) { Return(c, 1033); }
K(GetLocaleInfoW) {
    uint32_t buf = Arg(c, 2), size = Arg(c, 3);
    uint32_t tmp = HeapAlloc(256);
    uint32_t orig = buf;
    W32(c->esp + 12, tmp);
    W32(c->esp + 16, 256);
    uint32_t esp = c->esp;
    k_GetLocaleInfoA(c);
    W32(esp + 12, orig);
    W32(esp + 16, size);
    uint32_t n = c->eax;
    if (n) {
        std::string v(Str(tmp));
        bool fits;
        uint32_t r = PutWide(buf, size, v, &fits);
        c->eax = size == 0 ? r : fits ? r + 1 : 0;
    }
    HeapFree(tmp);
}
K(CompareStringW) {
    uint32_t flags = Arg(c, 1);
    int32_t n1 = int32_t(Arg(c, 3)), n2 = int32_t(Arg(c, 5));
    std::u16string a = ToUtf16(Wide(Arg(c, 2), n1)), b = ToUtf16(Wide(Arg(c, 4), n2));
    if (n1 < 0 && !a.empty() && a.back() == 0) a.pop_back();
    if (n2 < 0 && !b.empty() && b.back() == 0) b.pop_back();
    if (flags & 1) {  // NORM_IGNORECASE
        for (auto& ch : a) ch = char16_t(towlower(ch));
        for (auto& ch : b) ch = char16_t(towlower(ch));
    }
    int r = a.compare(b);
    Return(c, r < 0 ? 1u : r == 0 ? 2u : 3u, 6);  // CSTR_LESS_THAN / EQUAL / GREATER_THAN
}
K(LCMapStringW) {
    uint32_t flags = Arg(c, 1), src = Arg(c, 2), dst = Arg(c, 4), cap = Arg(c, 5);
    int32_t n = int32_t(Arg(c, 3));
    if (n < 0) {
        n = 0;
        while (R16(src + 2 * uint32_t(n))) ++n;
        ++n;
    }
    if (cap == 0) {
        Return(c, uint32_t(n), 6);
        return;
    }
    int32_t m = n < int32_t(cap) ? n : int32_t(cap);
    for (int32_t i = 0; i < m; ++i) {
        uint16_t ch = R16(src + 2 * uint32_t(i));
        if (flags & 0x100) ch = uint16_t(towlower(ch));  // LCMAP_LOWERCASE
        if (flags & 0x200) ch = uint16_t(towupper(ch));  // LCMAP_UPPERCASE
        W16(dst + 2 * uint32_t(i), ch);
    }
    Return(c, uint32_t(m), 6);
}
K(WideCharToMultiByte) {
    uint32_t cp = Arg(c, 0), src = Arg(c, 2), out = Arg(c, 4), cap = Arg(c, 5);
    int32_t n = int32_t(Arg(c, 3));
    std::string s;
    if (cp == 65001) {
        s = Wide(src, n);
        if (n < 0) s += '\0';
    } else {  // CP_ACP: Latin-1, the subset MultiByteToWideChar maps back
        for (int32_t i = 0;; ++i) {
            if (n >= 0 && i >= n) break;
            uint16_t ch = R16(src + 2 * uint32_t(i));
            s += ch < 0x100 ? char(ch) : '?';
            if (n < 0 && !ch) break;
        }
    }
    if (cap == 0) {
        Return(c, uint32_t(s.size()), 8);
        return;
    }
    uint32_t m = uint32_t(s.size()) < cap ? uint32_t(s.size()) : cap;
    memcpy(Str(out), s.data(), m);
    Return(c, m, 8);
}
K(CreateFileW) { CallAnsi(c, k_CreateFileA, {0}); }
K(GetFileAttributesW) { CallAnsi(c, k_GetFileAttributesA, {0}); }
K(SetFileAttributesW) { CallAnsi(c, k_SetFileAttributesA, {0}); }
K(DeleteFileW) { CallAnsi(c, k_DeleteFileA, {0}); }
K(CreateDirectoryW) { CallAnsi(c, k_CreateDirectoryA, {0}); }
K(RemoveDirectoryW) { CallAnsi(c, k_RemoveDirectoryA, {0}); }
K(CopyFileW) { CallAnsi(c, k_CopyFileA, {0, 1}); }
K(MoveFileExW) {
    // MOVEFILE_REPLACE_EXISTING: rename() replaces anyway.
    CallAnsi(c, k_MoveFileA, {0, 1}, 1);
}
K(LoadLibraryW) { CallAnsi(c, k_LoadLibraryA, {0}); }
K(GetModuleHandleW) { CallAnsi(c, k_GetModuleHandleA, {0}); }
K(GetModuleFileNameW) {
    std::string p = std::string(kGuestExeDir) + "\\SporeApp.exe";
    uint32_t buf = Arg(c, 1), size = Arg(c, 2);
    bool fits;
    uint32_t n = PutWide(buf, size, p, &fits);
    Return(c, fits ? n : size, 3);
}
// WIN32_FIND_DATAA (320 bytes) -> WIN32_FIND_DATAW (592): the header is the same, the names widen.
void WidenFindData(uint32_t a, uint32_t w) {
    memcpy(Ptr<void>(w), Ptr<void>(a), 44);
    PutWide(w + 44, 260, Str(a + 44));
    PutWide(w + 564, 14, Str(a + 304));
}
K(FindFirstFileW) {
    uint32_t out = Arg(c, 1), tmp = HeapAlloc(320);
    W32(c->esp + 8, tmp);
    uint32_t esp = c->esp;
    CallAnsi(c, k_FindFirstFileA, {0});
    W32(esp + 8, out);
    if (c->eax != kInvalidHandle) WidenFindData(tmp, out);
    HeapFree(tmp);
}
K(FindNextFileW) {
    uint32_t out = Arg(c, 1), tmp = HeapAlloc(320);
    W32(c->esp + 8, tmp);
    uint32_t esp = c->esp;
    k_FindNextFileA(c);
    W32(esp + 8, out);
    if (c->eax) WidenFindData(tmp, out);
    HeapFree(tmp);
}
K(GetFullPathNameW) {
    std::string full = FullGuestPath(Wide(Arg(c, 0)).c_str());
    uint32_t size = Arg(c, 1), buf = Arg(c, 2), part = Arg(c, 3);
    bool fits;
    uint32_t n = PutWide(buf, size, full, &fits);
    if (fits && part) {
        std::u16string w = ToUtf16(full.substr(0, full.find_last_of('\\') + 1));
        W32(part, buf + 2 * uint32_t(w.size()));
    }
    Return(c, n, 4);
}
K(GetLongPathNameW) {
    std::string p = Wide(Arg(c, 0));
    bool fits;
    uint32_t n = PutWide(Arg(c, 1), Arg(c, 2), p, &fits);
    Return(c, n, 3);
}
K(GetVolumePathNameW) {
    bool fits;
    PutWide(Arg(c, 1), Arg(c, 2), "C:\\", &fits);
    Return(c, fits ? 1 : 0, 3);
}
K(GetSystemDirectoryW) { Return(c, PutWide(Arg(c, 0), Arg(c, 1), "C:\\Windows\\System32"), 2); }
K(GetWindowsDirectoryW) { Return(c, PutWide(Arg(c, 0), Arg(c, 1), "C:\\Windows"), 2); }
K(GetTempPathW) {
    mkdir(HostPath("C:\\Temp").c_str(), 0755);
    Return(c, PutWide(Arg(c, 1), Arg(c, 0), "C:\\Temp\\"), 2);
}
K(GetVolumeInformationW) {
    uint32_t name = Arg(c, 1), name_size = Arg(c, 2), fs = Arg(c, 6), fs_size = Arg(c, 7);
    uint32_t tname = name ? HeapAlloc(256) : 0, tfs = fs ? HeapAlloc(256) : 0;
    W32(c->esp + 8, tname);
    W32(c->esp + 12, tname ? 256 : 0);
    W32(c->esp + 28, tfs);
    W32(c->esp + 32, tfs ? 256 : 0);
    uint32_t esp = c->esp;
    CallAnsi(c, k_GetVolumeInformationA, {0});
    W32(esp + 8, name);
    W32(esp + 12, name_size);
    W32(esp + 28, fs);
    W32(esp + 32, fs_size);
    if (c->eax) {
        if (name) PutWide(name, name_size, Str(tname));
        if (fs) PutWide(fs, fs_size, Str(tfs));
    }
    if (tname) HeapFree(tname);
    if (tfs) HeapFree(tfs);
}
K(CancelIo) { Return(c, 1, 1); }
// Directory change notifications: not available (the game then rescans on its own schedule).
K(FindFirstChangeNotificationW) {
    SetLastError(ERROR_NOT_SUPPORTED);
    Return(c, kInvalidHandle, 3);
}
K(FindNextChangeNotification) { Return(c, 0, 1); }
K(FindCloseChangeNotification) { Return(c, 1, 1); }
K(EnumResourceNamesA) {
    Log("EnumResourceNamesA(type %08x): no resources", Arg(c, 1));
    SetLastError(ERROR_RESOURCE_TYPE_NOT_FOUND);
    Return(c, 0, 4);
}
std::map<std::string, std::string> g_env;  // SetEnvironmentVariableA; under g_env_mutex
std::mutex g_env_mutex;
K(SetEnvironmentVariableA) {
    std::lock_guard<std::mutex> lock(g_env_mutex);
    if (Arg(c, 1)) g_env[Str(Arg(c, 0))] = Str(Arg(c, 1));
    else g_env.erase(Str(Arg(c, 0)));
    Return(c, 1, 2);
}
K(GetEnvironmentVariableA) {
    std::lock_guard<std::mutex> lock(g_env_mutex);
    auto it = g_env.find(Str(Arg(c, 0)));
    if (it == g_env.end()) {
        SetLastError(ERROR_ENVVAR_NOT_FOUND);
        Return(c, 0, 3);
        return;
    }
    uint32_t buf = Arg(c, 1), size = Arg(c, 2);
    if (it->second.size() + 1 > size) {
        Return(c, uint32_t(it->second.size() + 1), 3);
        return;
    }
    strcpy(Str(buf), it->second.c_str());
    Return(c, uint32_t(it->second.size()), 3);
}

#undef K

const ImportDef kKernel32[] = {
    E(GetStartupInfoA), E(GetCommandLineA), E(GetStdHandle), E(GetFileType), E(FlushConsoleInputBuffer), E(GetVersion), E(GetVersionExW), E(SetLastError), E(InterlockedIncrement), E(InterlockedExchangeAdd), E(HeapAlloc), E(LocalAlloc), E(LocalFree), E(GlobalAlloc), E(GlobalLock), E(GlobalUnlock), E(GlobalFree), E(GlobalSize), E(GlobalMemoryStatusEx), E(GetSystemInfo), E(SetPriorityClass), E(GetPriorityClass), E(GetThreadPriority), E(SetProcessAffinityMask), E(SetThreadIdealProcessor), E(SetThreadExecutionState), E(GetComputerNameA), E(GetComputerNameExA), E(OpenProcess), E(GetProcessTimes), E(DuplicateHandle), E(SleepEx), E(QueueUserAPC), E(WaitForMultipleObjectsEx), E(ReleaseMutex), E(InitializeCriticalSectionAndSpinCount), E(VirtualQuery), E(GetSystemTime), E(SystemTimeToFileTime), E(GetSystemDefaultLCID), E(GetLocaleInfoW), E(CompareStringW), E(LCMapStringW), E(WideCharToMultiByte), E(CreateFileW), E(GetFileAttributesW), E(SetFileAttributesW), E(DeleteFileW), E(CreateDirectoryW), E(RemoveDirectoryW), E(CopyFileW), E(MoveFileExW), E(LoadLibraryW), E(GetModuleHandleW), E(GetModuleFileNameW), E(FindFirstFileW), E(FindNextFileW), E(GetFullPathNameW), E(GetLongPathNameW), E(GetVolumePathNameW), E(GetSystemDirectoryW), E(GetWindowsDirectoryW), E(GetTempPathW), E(GetVolumeInformationW), E(CancelIo), E(FindFirstChangeNotificationW), E(FindNextChangeNotification), E(FindCloseChangeNotification), E(EnumResourceNamesA), E(SetEnvironmentVariableA), E(GetEnvironmentVariableA),
    E(GetCurrentProcessId), E(GetCurrentThreadId), E(GetCurrentProcess), E(GetCurrentThread), E(GetTickCount),
    E(QueryPerformanceCounter), E(QueryPerformanceFrequency), E(GetSystemTimeAsFileTime), E(GetLocalTime),
    E(GetTimeZoneInformation), E(InterlockedCompareExchange), E(InterlockedExchange), E(InterlockedDecrement),
    E(VirtualAlloc), E(VirtualFree), E(GetProcessHeap), E(HeapFree), E(GlobalMemoryStatus), E(GetModuleHandleA),
    E(LoadLibraryA), E(FreeLibrary), E(GetProcAddress), E(GetModuleFileNameA), E(GetVersionExA),
    E(IsDebuggerPresent), E(SetErrorMode), E(SetUnhandledExceptionFilter), E(UnhandledExceptionFilter),
    E(OutputDebugStringA), E(RaiseException), E(TerminateProcess), E(GetLastError), E(FormatMessageA),
    E(CreateThread), E(ResumeThread), E(SuspendThread), E(SetThreadPriority), E(SetThreadAffinityMask),
    E(GetProcessAffinityMask), E(GetExitCodeThread), E(TerminateThread), E(ExitThread), E(Sleep),
    E(ConvertThreadToFiber), E(CreateFiber), E(SwitchToFiber), E(DeleteFiber), E(ConvertFiberToThread),
    E(TlsAlloc), E(TlsFree), E(TlsGetValue), E(TlsSetValue), E(InitializeCriticalSection),
    E(DeleteCriticalSection), E(EnterCriticalSection), E(LeaveCriticalSection), E(TryEnterCriticalSection),
    E(CreateEventA), E(SetEvent), E(ResetEvent), E(CreateSemaphoreA), E(ReleaseSemaphore), E(CreateMutexA),
    E(WaitForSingleObject), E(WaitForMultipleObjects), E(CloseHandle), E(CreateFileA), E(ReadFile), E(WriteFile),
    E(SetFilePointer), E(GetFileSize), E(SetEndOfFile), E(FlushFileBuffers), E(GetFileTime), E(CompareFileTime),
    E(DeleteFileA), E(MoveFileA), E(CopyFileA), E(GetFileAttributesA), E(SetFileAttributesA), E(CreateDirectoryA),
    E(RemoveDirectoryA), E(FindFirstFileA), E(FindNextFileA), E(FindClose), E(GetFullPathNameA),
    E(GetCurrentDirectoryA), E(SetCurrentDirectoryA), E(GetDriveTypeA), E(GetLogicalDrives),
    E(GetVolumeInformationA), E(GetDiskFreeSpaceExA), E(DeviceIoControl), E(OpenFile), E(MultiByteToWideChar),
    E(lstrlenW), E(GetACP), E(GetThreadLocale), E(GetSystemDefaultLangID), E(GetLocaleInfoA),
};
#undef E
ImportRegistrar g_kernel32(kKernel32);

}  // namespace

// SetEvent for host code (dsound.cpp position notifications): signals an event handle from any host thread.
void SignalEventHandle(uint32_t handle) {
    auto* e = static_cast<Event*>(HandleObject(handle));
    std::lock_guard<std::mutex> lock(g_kernel_mutex);
    if (e && e->type == KType::Event) {
        e->state = true;
        e->Notify();
    }
}

// _beginthreadex (msvcrt.cpp): CreateThread's arguments in the same order, but cdecl (the caller pops them).
void BeginThreadEx(Ctx* c) {
    k_CreateThread(c);
    c->esp -= 24;
}
// The _wstat-style helpers of the CRT use the same conversion as the wide kernel32 functions.
std::string WideToUtf8(uint32_t a, int32_t n) { return Wide(a, n); }
uint32_t PutWideString(uint32_t buf, uint32_t cap, const std::string& s) { return PutWide(buf, cap, s); }

}  // namespace native
