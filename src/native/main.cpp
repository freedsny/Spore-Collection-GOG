// Native host entry point: replaces Windows' process startup for SporeApp.exe. Maps the exe into guest memory at its
// preferred base, gives the main thread its TEB and static TLS, runs the TLS callbacks and then the exe's own entry
// point (the MSVC CRT startup, which calls WinMain). From there the game drives itself: it creates its window,
// runs its message loop (user32.cpp turns SDL events into window messages) and ends through ExitProcess.
#include <SDL2/SDL.h>
#include <SDL2/SDL_main.h>  // Android: main() becomes SDL_main, called by SDLActivity
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sched.h>
#include <signal.h>
#include <ucontext.h>
#include <unwind.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#include <jni.h>
#endif

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "guest.h"
#include "win.h"

#ifdef SPORERE_PGO_GEN
extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char*);
extern "C" void __llvm_profile_reset_counters(void);
#endif

namespace native {

const char* g_game_dir = nullptr;
const char* g_data_dir = nullptr;  // Android: the app's external files folder (logs, ini)
const char* g_user_root = nullptr;  // the guest C: drive when not the default (PrepareUserRoot)

namespace {
std::mutex g_log_mutex;
FILE* g_log = nullptr;
}  // namespace

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    va_list ap;
    va_start(ap, fmt);
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_INFO, "sporere", fmt, ap);
#else
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
#endif
    va_end(ap);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
}

void VulkanDriverAbandon();  // vulkan_driver.cpp
void VulkanDriverCrashed();

namespace {

// Crash report: the fault and the faulting code's library + offset go to the log (and logcat), then the previous
// handler (Android's debuggerd) takes over. Without it a crash on a user's phone just cuts the log off. Formatting
// is not strictly async-signal-safe, which is acceptable for a process that is dying anyway.
struct sigaction g_old_actions[32];

void CrashWrite(const char* text) {
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_ERROR, "sporere", text);
#else
    write(2, text, strlen(text));
    write(2, "\n", 1);
#endif
    if (g_log) {
        int fd = fileno(g_log);
        write(fd, text, strlen(text));
        write(fd, "\n", 1);
        fsync(fd);
    }
}

void DescribeAddress(char* out, size_t size, const char* what, uintptr_t address) {
    Dl_info info{};
    if (address && dladdr(reinterpret_cast<void*>(address), &info) && info.dli_fname) {
        const char* lib = strrchr(info.dli_fname, '/');
        snprintf(out, size, "crash: %s %#lx = %s+%#lx (%s)", what, (unsigned long)address, lib ? lib + 1 : info.dli_fname,
                 (unsigned long)(address - reinterpret_cast<uintptr_t>(info.dli_fbase)),
                 info.dli_sname ? info.dli_sname : "?");
    } else {
        snprintf(out, size, "crash: %s %#lx", what, (unsigned long)address);
    }
}

void OnCrash(int sig, siginfo_t* info, void* context) {
    VulkanDriverCrashed();  // a crash before the driver is confirmed: auto mode avoids Turnip next time
    char line[512];
    snprintf(line, sizeof(line), "crash: signal %d (%s) code %d addr %p on tid %d", sig, strsignal(sig), info->si_code,
             info->si_addr, int(gettid()));
    CrashWrite(line);
#if defined(__aarch64__)
    const auto& mc = static_cast<ucontext_t*>(context)->uc_mcontext;
    DescribeAddress(line, sizeof(line), "pc", mc.pc);
    CrashWrite(line);
    DescribeAddress(line, sizeof(line), "lr", mc.regs[30]);
    CrashWrite(line);
#elif defined(__x86_64__)
    DescribeAddress(line, sizeof(line), "pc", static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
    CrashWrite(line);
#endif
    // The callers: for abort() the pc and lr above are both in libc, the caller that gave up is further up.
    struct Walk {
        char* line;
        size_t size;
        int depth;
    } walk = {line, sizeof(line), 0};
    _Unwind_Backtrace(
        [](_Unwind_Context* ctx, void* arg) -> _Unwind_Reason_Code {
            auto* w = static_cast<Walk*>(arg);
            uintptr_t pc = _Unwind_GetIP(ctx);
            if (!pc || w->depth >= 24) return _URC_END_OF_STACK;
            char what[16];
            snprintf(what, sizeof(what), "#%02d", w->depth++);
            DescribeAddress(w->line, w->size, what, pc);
            CrashWrite(w->line);
            return _URC_NO_REASON;
        },
        &walk);
    sigaction(sig, &g_old_actions[sig], nullptr);
    if (info->si_code <= 0) raise(sig);  // sent by kill/tgkill/abort: deliver again; a real fault re-runs on return
}

void InstallCrashHandler() {
    struct sigaction sa {};
    sa.sa_sigaction = OnCrash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, &g_old_actions[sig]);
}

}  // namespace

std::string TakeIoStats();   // kernel32.cpp
std::string ArenaUsage();    // memory.cpp

void Fatal(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log("FATAL: %s", buf);
    VulkanDriverAbandon();  // not the driver's fault: the next start keeps trying Turnip
    _exit(70);
}

// A start that cannot go on: tell the user and end the process. Returning from main would let SDL run main again in
// the same process, on top of this run's guest memory.
[[noreturn]] void StartupFailure(const char* what) {
    Log("startup failed: %s", what);
    VulkanDriverAbandon();
    std::string msg = std::string(what) + "\n\nDetails are in spore-native.log.";
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Spore", msg.c_str(), nullptr);
    _exit(1);
}

uint32_t HostWindow();  // user32.cpp
void InitClientSize(int window_w, int window_h);
void StartWatchdog(std::atomic<uint64_t>* frames);  // watchdog.cpp
extern std::atomic<uint64_t> g_frames;  // d3d9.cpp
void InitD3D9Bridge();  // d3d9.cpp
void SelectVulkanDriver();  // vulkan_driver.cpp
void VulkanDriverConfirm();
void InitInput();  // input.cpp
void StartProfiler();  // profiler.cpp
void ForwardInput(const SDL_Event& e);
void InjectKey(SDL_Scancode sc, bool down);
extern SDL_Window* g_sdl_window;

}  // namespace native


// ---- performance counters and frame limiter (read by the on-screen overlay through JNI) ----
namespace native {
std::atomic<float> g_fps{0}, g_frame_ms{0};
std::atomic<int> g_fps_limit{0};  // 0 = no limit (the swap interval still applies)

static long ResidentMiB() {
    long pages = 0, resident = 0;
    if (FILE* f = fopen("/proc/self/statm", "r")) {
        if (fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        fclose(f);
    }
    return resident * (sysconf(_SC_PAGESIZE) / 1024) / 1024;
}
}  // namespace native


// big.LITTLE: keep the engine's main thread on the fast cores (cpu_capacity >= half of the largest). The scheduler
// usually gets there by itself, but a frame-bound thread that waits on the GPU looks idle to it now and then and
// lands on a small core for a while. Setting big_cores (Android default 1).
static void PinToBigCores() {
#ifdef __ANDROID__
    // battery_saver: the scheduler picks the cores (the biggest ones draw several times the power per clock)
    if (!native::SettingInt("big_cores", 1) || native::SettingInt("battery_saver", 0)) return;
    int caps[64] = {}, max_cap = 0, n = 0;
    for (; n < 64; ++n) {
        char path[96];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", n);
        FILE* f = fopen(path, "r");
        if (!f) break;
        if (fscanf(f, "%d", &caps[n]) != 1) caps[n] = 0;
        fclose(f);
        if (caps[n] > max_cap) max_cap = caps[n];
    }
    if (n == 0 || max_cap == 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    int big = 0;
    for (int i = 0; i < n; ++i) {
        if (caps[i] * 2 >= max_cap) {
            CPU_SET(i, &set);
            ++big;
        }
    }
    if (big == 0 || big == n) return;  // not big.LITTLE
    int r = sched_setaffinity(0, sizeof(set), &set);
    native::Log("main thread pinned to %d of %d cores%s", big, n, r == 0 ? "" : " (failed)");
#endif
}

using namespace native;

#ifdef __ANDROID__
// DXVK and FAudio report errors on stderr, which goes nowhere in an app: forward it to logcat (tag sporere-err).
static void RedirectStderrToLogcat() {
    static int fds[2];
    if (pipe(fds) != 0) return;
    dup2(fds[1], 2);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    std::thread([] {
        char buf[1024];
        std::string line;
        for (ssize_t n; (n = read(fds[0], buf, sizeof(buf))) > 0;) {
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') {
                    __android_log_write(ANDROID_LOG_WARN, "sporere-err", line.c_str());
                    line.clear();
                } else {
                    line += buf[i];
                }
            }
        }
    }).detach();
}
#endif

// The port's credit. On Android the Java side hands over its own copy (SetupActivity.CREDIT); a build where either
// was removed or changed does not start.
constexpr const char* kCredit = "Spore Android port by VinTeRuS";

#ifdef __ANDROID__
// ---- the guest C: drive on a phone ----
// Saves, creations and temp files used to live in the app's own folder (Android/data/com.sporere.game/files/native_c).
// Files other apps put there (a save copied with a file manager, Shizuku, a PC) belong to another user: the app can
// often neither read nor write them, and the game crashed on start. They now live next to the game, in
// <game>/UserData, which any file manager can fill and the app (all files access) can always use.

bool CopyFileTo(const std::string& from, const std::string& to) {
    int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    int out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    bool ok = out >= 0;
    char buf[1 << 16];
    for (ssize_t n; ok && (n = read(in, buf, sizeof buf)) > 0;) ok = write(out, buf, size_t(n)) == n;
    close(in);
    if (out >= 0) close(out);
    if (!ok) unlink(to.c_str());
    return ok;
}

void CopyTree(const std::string& from, const std::string& to, std::vector<std::string>& failed) {
    mkdir(to.c_str(), 0755);
    DIR* d = opendir(from.c_str());
    if (!d) {
        failed.push_back(from);
        return;
    }
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string a = from + "/" + name, b = to + "/" + name;
        struct stat st;
        if (stat(a.c_str(), &st) != 0) {
            failed.push_back(a);
        } else if (S_ISDIR(st.st_mode)) {
            CopyTree(a, b, failed);
        } else if (S_ISREG(st.st_mode) && !CopyFileTo(a, b)) {
            failed.push_back(a);
        }
    }
    closedir(d);
}

// Files under `dir` the app cannot open for reading and writing (the game opens its profile files that way).
void FindUnusable(const std::string& dir, std::vector<std::string>& bad, int depth = 0) {
    DIR* d = opendir(dir.c_str());
    if (!d) {
        bad.push_back(dir);
        return;
    }
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string p = dir + "/" + name;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) {
            bad.push_back(p);
        } else if (S_ISDIR(st.st_mode)) {
            if (depth < 6) FindUnusable(p, bad, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            int fd = open(p.c_str(), O_RDWR | O_CLOEXEC);
            if (fd < 0) bad.push_back(p);
            else close(fd);
        }
    }
    closedir(d);
}

std::string ShortList(const std::vector<std::string>& paths) {
    std::string s;
    for (size_t i = 0; i < paths.size() && i < 5; ++i) s += "\n" + paths[i];
    if (paths.size() > 5) s += "\n(" + std::to_string(paths.size() - 5) + " more)";
    return s;
}

// Picks the C: drive (returns "" to keep the app's folder) and moves the saves there once.
std::string PrepareUserRoot() {
    const std::string old = std::string(g_data_dir) + "/native_c", game = g_game_dir;
    if (game.compare(0, strlen(g_data_dir), g_data_dir) == 0) return "";  // the game is in the app's folder anyway
    const std::string root = game + "/UserData";
    mkdir(root.c_str(), 0755);
    const std::string probe = root + "/.sporere-write-test";
    int fd = open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        Log("saves: %s is not writable (%s), they stay in %s", root.c_str(), strerror(errno), old.c_str());
        return "";
    }
    close(fd);
    unlink(probe.c_str());
    struct stat st;
    if (stat((root + "/Users").c_str(), &st) != 0 && stat((old + "/Users").c_str(), &st) == 0) {
        std::vector<std::string> failed;
        CopyTree(old, root, failed);
        Log("saves: copied %s to %s (%zu could not be read)", old.c_str(), root.c_str(), failed.size());
        for (size_t i = 0; i < failed.size() && i < 50; ++i) Log("saves: could not copy %s", failed[i].c_str());
        if (!failed.empty()) {
            std::string msg = "Your saves moved to " + root + " (a folder any file manager can reach).\n\n"
                              "These files could not be read and were left out (another app put them into the "
                              "app's folder):" + ShortList(failed) + "\n\nTo bring them back, copy them into " +
                              root + " with a file manager.";
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, kCredit, msg.c_str(), nullptr);
        }
    }
    Log("saves: %s", root.c_str());
    return root;
}

// Warns when the game will not be able to use its profile: it crashes on start then.
void CheckUserRoot(const std::string& root) {
    std::vector<std::string> bad;
    std::string spore = root + "/Users/Player/AppData/Roaming/Spore";
    struct stat st;
    if (stat(spore.c_str(), &st) == 0) FindUnusable(spore, bad);
    mkdir((root + "/Temp").c_str(), 0755);
    std::string probe = root + "/Temp/.sporere-write-test";
    int fd = open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) bad.push_back(root + "/Temp");
    else close(fd), unlink(probe.c_str());
    if (bad.empty()) return;
    for (size_t i = 0; i < bad.size() && i < 50; ++i) Log("saves: not usable: %s", bad[i].c_str());
    std::string msg = "The game can't use these save files (another app put them there and the game may not read "
                      "or write them):" + ShortList(bad) + "\n\nDelete them with the app that copied them, or copy "
                      "your saves with a file manager into " + root + ". The game may close on start until then.";
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, kCredit, msg.c_str(), nullptr);
}
#endif


int main(int argc, char** argv) {
#ifdef __ANDROID__
    RedirectStderrToLogcat();
    setenv("DXVK_LOG_LEVEL", "info", 0);
    if (const char* credit = getenv("SPORERE_CREDIT"); !credit || strcmp(credit, kCredit) != 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kCredit,
                                 "This build of the Spore Android port was modified: the author's credit is missing.\n"
                                 "The port is made by VinTeRuS - patreon.com/cw/VinTeRuS",
                                 nullptr);
        return 1;
    }
#endif
    // The game root: the folder with SporebinEP1/, Data/, DataEP1/ (C:\Game for the guest).
    g_game_dir = argc > 1 ? argv[1] : getenv("SPORERE_GAME");
#ifdef __ANDROID__
    // Logs, sporere.ini, saves (native_c) and the DXVK caches live in the app's external files folder
    // (/sdcard/Android/data/com.sporere.game/files); the game is <that folder>/game unless SetupActivity found it
    // elsewhere (SPORERE_GAME).
    static std::string android_game;
    const char* base = SDL_AndroidGetExternalStoragePath();
    if (!base) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kCredit, SDL_GetError(), nullptr);
        return 1;
    }
    if (chdir(base) != 0) return 1;
    g_data_dir = base;
#ifdef SPORERE_PGO_GEN
    // Instrumented build (make PGO=gen): the counters go to <files>/spore-pgo-*.profraw every minute, merged into
    // the file and reset, since an Android app is usually killed rather than exited.
    {
        __llvm_profile_set_filename((std::string(base) + "/spore-pgo-%m.profraw").c_str());
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(60));
                int r = __llvm_profile_write_file();
                __llvm_profile_reset_counters();
                Log("pgo: profile written (%d)", r);
            }
        }).detach();
        Log("pgo: instrumented build, profile every 60 s");
    }
#endif
    setenv("DXVK_STATE_CACHE_PATH", base, 0);
    setenv("DXVK_LOG_PATH", base, 0);
    if (!g_game_dir) {
        android_game = std::string(base) + "/game";
        g_game_dir = android_game.c_str();
    }
    if (std::string exe = std::string(g_game_dir) + "/SporebinEP1/SporeApp.exe"; access(exe.c_str(), R_OK) != 0) {
        std::string msg = "Cannot read " + exe + " (" + strerror(errno) + ").\nCopy the game files there.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kCredit, msg.c_str(), nullptr);
        return 1;
    }
#endif
    if (!g_game_dir) {
        fprintf(stderr, "usage: %s <game directory>\n", argv[0]);
        return 2;
    }
    rename("spore-native.log", "spore-native.prev.log");  // the previous run's log survives one restart
    g_log = fopen("spore-native.log", "w");
    InstallCrashHandler();
#ifdef __ANDROID__
    {
        static const std::string user_root = PrepareUserRoot();
        if (!user_root.empty()) g_user_root = user_root.c_str();
        CheckUserRoot(g_user_root ? g_user_root : std::string(g_data_dir) + "/native_c");
    }
#endif
    Log("%s - build %s %s", kCredit, __DATE__, __TIME__);
    InitGuestMemory();
    GuestThread* main_thread = AttachThread(1u << 20);  // the exe's SizeOfStackReserve
    StartProfiler();  // cpu_profile = 1: sampled hot spots in the log
    PinToBigCores();  // the game's main thread runs its whole frame: keep it on the fast cores
    // ADPF: the frame budget from fps_limit (60 when unlimited); Present reports each frame's work (d3d9.cpp).
    {
        int limit = SettingInt("fps_limit", 0);
        PerfHintStart(limit > 0 ? limit : 60);
    }
    InitD3D9Bridge();
    setenv("DXVK_WSI_DRIVER", "SDL2", 0);  // DXVK native: the HWNDs we pass are SDL windows
    // The frame is drawn at the render size and stretched to the screen when presented; sharpen = 0-10 makes that
    // stretch a sharpening upscale (android/patches/dxvk-sharpen.patch): a lower render size, so a bigger game
    // interface, still looks crisp. 0 = plain bilinear.
    setenv("DXVK_PRESENT_SHARPEN", std::to_string(SettingInt("sharpen", 5)).c_str(), 1);
    SelectVulkanDriver();
    if (SettingInt("present_stats", 0)) setenv("DXVK_PRESENT_STATS", "1", 0);
    std::string dc = Setting("dxvk_config", "");
#ifdef __ANDROID__
    // Spore presents without vsync (D3DPRESENT_INTERVAL_IMMEDIATE): on a phone frames then reach the screen unevenly
    // (judder, tearing). Vsync with triple buffering paces them to the display; dxvk_config overrides it.
    if (dc.empty()) dc = "d3d9.presentInterval = 1, d3d9.numBackBuffers = 3";
#endif
    if (!dc.empty()) {
        for (char& ch : dc) ch = ch == ',' ? ';' : ch;
        setenv("DXVK_CONFIG", dc.c_str(), 0);
    }
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) Fatal("SDL_Init: %s", SDL_GetError());
    // The window exists before the game asks for one: CreateWindowExW hands out this one (user32.cpp).
    g_sdl_window = SDL_CreateWindow("Spore - port by VinTeRuS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                    SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
#ifdef __ANDROID__
                                        | SDL_WINDOW_FULLSCREEN
#endif
    );
    if (!g_sdl_window) Fatal("SDL_CreateWindow: %s", SDL_GetError());
    // Typed text reaches the game as WM_CHAR from SDL text input events. SDL disables those on phones until the
    // on-screen keyboard is opened through SDL; the port opens it (and types the cheat console's commands) itself,
    // so the events stay on without showing a keyboard.
    SDL_EventState(SDL_TEXTINPUT, SDL_ENABLE);
    int window_w, window_h;
    SDL_GetWindowSize(g_sdl_window, &window_w, &window_h);
#ifdef __ANDROID__
    SDL_DisplayMode mode;  // the window becomes the whole screen; its size is not known yet
    if (SDL_GetDesktopDisplayMode(0, &mode) == 0) {
        window_w = mode.w > mode.h ? mode.w : mode.h;  // landscape
        window_h = mode.w > mode.h ? mode.h : mode.w;
    }
    // Settings > Screen 16:9 / Safe area: the game's surface is smaller than the screen (SporeControls.applyScreen);
    // rendering at the screen's proportions squashed the picture into it
    if (const char* s = getenv("SPORERE_SURFACE_SIZE")) {
        int sw = 0, sh = 0;
        if (sscanf(s, "%dx%d", &sw, &sh) == 2 && sw > 0 && sh > 0) window_w = sw, window_h = sh;
    }
#endif
    InitClientSize(window_w, window_h);

    std::string exe = std::string(g_game_dir) + "/SporebinEP1/SporeApp.exe";
    if (!LoadImage(exe.c_str(), &g_engine, kEngineBase)) StartupFailure("Cannot load SporebinEP1/SporeApp.exe.");
    g_engine_lo = g_engine.base;
    g_engine_hi = g_engine.base + g_engine.size;
    SetupStaticTls(main_thread);  // the main thread was attached before the exe (and its TLS template) existed
    RecompLookup(g_engine.entry);  // builds the dispatch table before any guest thread runs

    StartWatchdog(&g_frames);  // counted by Present (d3d9.cpp)
    // TLS callbacks (DLL_PROCESS_ATTACH), as the Windows loader runs them before the entry point.
    for (uint32_t cb = g_static_tls.callbacks; cb && R32(cb); cb += 4) {
        Log("TLS callback %08x", R32(cb));
        GuestCall(R32(cb), {g_engine.base, 1, 0});
    }
    Log("entry point %08x", g_engine.entry);
    uint32_t code = GuestCall(g_engine.entry, {});
    // The CRT startup normally leaves through ExitProcess; returning means it gave up.
    Log("entry point returned %u", code);
    return int(code);
}
