// D3D9 for the native host: the engine's COM calls are forwarded to DXVK (D3D9 on Vulkan, built natively).
//
// Guest objects are 16-byte blocks in guest memory: [0] guest vtable (thunk addresses), [4] index of the host
// object. Method handlers are generated from d3d9.h (tools/gen_d3d9_bridge.py -> d3d9_gen.inc); this file holds the
// object table and the methods whose arguments need conversion: structures with window handles (present
// parameters) and Lock/Unlock, where the host's mapped pointer lies outside guest memory and a guest shadow buffer
// is copied in and out.
#include <SDL2/SDL.h>
#include <d3d9.h>

#include <unistd.h>
#ifdef __ANDROID__
#include <jni.h>
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "../host/d3d9_trace.h"
#include "win.h"

namespace native {
std::string TakeIoStats();  // kernel32.cpp: reads since the last perf line
extern std::atomic<uint32_t> g_game_mode;  // the active game mode (UpdateGameMode, below)
extern int g_trace_draws;                  // trace_hm_draws (TraceDraw, below)
extern std::atomic<int> g_fps_limit;       // main.cpp: frame rate cap, 0 = none

SDL_Window* g_sdl_window = nullptr;
uint32_t HostWindow();
void ClientSize(int* w, int* h);     // user32.cpp
void SetClientSize(int w, int h);

namespace {

struct MethodDef {
    const char* name;
    ImportFn fn;
};
struct InterfaceDef {
    const char* name;
    const MethodDef* methods;
    size_t count;
};

std::recursive_mutex g_mutex;
// index -> host object, read without a lock by every bridged call: fixed chunks that never move, appended to
// under g_mutex.
class HostTable {
public:
    void* Get(uint32_t i) const {
        return chunks_[i >> kBits].load(std::memory_order_acquire)[i & kMask].load(std::memory_order_acquire);
    }
    void Set(uint32_t i, void* p) {
        chunks_[i >> kBits].load(std::memory_order_relaxed)[i & kMask].store(p, std::memory_order_release);
    }
    // True when i names a slot that was ever handed out (a guest "object" that is no wrapper of ours has garbage there).
    bool Valid(uint32_t i) const {
        return i < size_ && chunks_[i >> kBits].load(std::memory_order_acquire) != nullptr;
    }
    uint32_t Append(void* p) {
        uint32_t i = size_;
        if ((i >> kBits) >= kChunks) Fatal("d3d9: too many objects");
        if (!chunks_[i >> kBits].load(std::memory_order_relaxed)) {
            chunks_[i >> kBits].store(new std::atomic<void*>[kMask + 1](), std::memory_order_release);
        }
        Set(i, p);
        ++size_;
        return i;
    }

private:
    static constexpr uint32_t kBits = 14, kMask = (1u << kBits) - 1, kChunks = 4096;
    std::atomic<std::atomic<void*>*> chunks_[kChunks] = {};
    uint32_t size_ = 0;
};
HostTable g_hosts;
// host object -> guest object. Child surfaces die with their texture without a Release through the bridge, so an
// entry can be stale: a new object at the same address with another interface must not reuse it.
std::unordered_map<void*, uint32_t> g_guest_of;
std::map<std::string, uint32_t> g_vtables;       // interface name -> guest vtable

template <typename T>
T* Host(uint32_t guest) {
    return static_cast<T*>(g_hosts.Get(R32(guest + 4)));
}

// An object pointer the game passes as an argument (SetTexture, SetStreamSource, ...). Spore sometimes hands over a
// stale raster's pointer that is no wrapper at all: logged and treated as NULL (valid for those calls) instead of
// reading a host slot out of garbage.
template <typename T>
T* Unwrap(uint32_t guest) {
    if (!guest) return nullptr;
    uint32_t index = R32(guest + 4);
    if (!g_hosts.Valid(index)) {
        static std::atomic<int> logged{0};
        if (logged++ < 20) Log("d3d9: %08x is not a D3D object (vtable %08x, index %08x); used as NULL", guest, R32(guest), index);
        return nullptr;
    }
    return Host<T>(guest);
}

uint32_t WrapAs(void* host, const char* iface) {
    if (!host) return 0;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto vt = g_vtables.find(iface);
    if (vt == g_vtables.end()) Fatal("d3d9: no vtable for %s", iface);
    auto it = g_guest_of.find(host);
    if (it != g_guest_of.end() && R32(it->second) == vt->second) return it->second;
    uint32_t g = HeapAlloc(16);
    W32(g, vt->second);
    W32(g + 4, g_hosts.Append(host));
    g_guest_of[host] = g;
    return g;
}

void DeadQueryInterface(Ctx* c) {
    if (Arg(c, 2)) W32(Arg(c, 2), 0);
    Return(c, E_NOINTERFACE, 3);
}
void DeadAddRef(Ctx* c) { Return(c, 0, 1); }
void DeadRelease(Ctx* c) {
    static int logged = 0;
    if (logged++ < 20) Log("d3d9: Release of an already released object %08x (from %08x) ignored", Arg(c, 0), R32(c->esp));
    Return(c, 0, 1);
}
void DeadMethod(Ctx* c) { Fatal("d3d9: method call on a released object %08x (from %08x)", Arg(c, 0), R32(c->esp)); }
uint32_t DeadVtable() {
    static uint32_t vt = [] {
        static const ImportDef defs[] = {{"d3d9.com", "Dead::QueryInterface", DeadQueryInterface},
                                         {"d3d9.com", "Dead::AddRef", DeadAddRef},
                                         {"d3d9.com", "Dead::Release", DeadRelease},
                                         {"d3d9.com", "Dead::Method", DeadMethod}};
        RegisterImports(defs, 4);
        uint32_t t = HeapAlloc(4 * 128);
        for (uint32_t k = 0; k < 128; ++k) W32(t + 4 * k, ImportThunk("d3d9.com", defs[k < 3 ? k : 3].name));
        return t;
    }();
    return vt;
}

void Forget(uint32_t guest) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    uint32_t index = R32(guest + 4);
    auto it = g_guest_of.find(g_hosts.Get(index));
    if (it != g_guest_of.end() && it->second == guest) g_guest_of.erase(it);
    g_hosts.Set(index, nullptr);
    // The guest block is not reused. Its vtable becomes the dead one: a late Release or AddRef from the game (the
    // engine releases some rasters once more than it acquired, which Windows tolerates for objects the device still
    // holds) does nothing, and any other method stops with a clear message instead of jumping through garbage.
    W32(guest, DeadVtable());
}

uint32_t Wrap(IDirect3D9* p) { return WrapAs(p, "IDirect3D9"); }
uint32_t Wrap(IDirect3DDevice9* p) { return WrapAs(p, "IDirect3DDevice9"); }
uint32_t Wrap(IDirect3DSwapChain9* p) { return WrapAs(p, "IDirect3DSwapChain9"); }
uint32_t Wrap(IDirect3DSurface9* p) { return WrapAs(p, "IDirect3DSurface9"); }
uint32_t Wrap(IDirect3DVolume9* p) { return WrapAs(p, "IDirect3DVolume9"); }
uint32_t Wrap(IDirect3DVertexBuffer9* p) { return WrapAs(p, "IDirect3DVertexBuffer9"); }
uint32_t Wrap(IDirect3DIndexBuffer9* p) { return WrapAs(p, "IDirect3DIndexBuffer9"); }
uint32_t Wrap(IDirect3DTexture9* p) { return WrapAs(p, "IDirect3DTexture9"); }
uint32_t Wrap(IDirect3DCubeTexture9* p) { return WrapAs(p, "IDirect3DCubeTexture9"); }
uint32_t Wrap(IDirect3DVolumeTexture9* p) { return WrapAs(p, "IDirect3DVolumeTexture9"); }
uint32_t Wrap(IDirect3DVertexDeclaration9* p) { return WrapAs(p, "IDirect3DVertexDeclaration9"); }
uint32_t Wrap(IDirect3DVertexShader9* p) { return WrapAs(p, "IDirect3DVertexShader9"); }
uint32_t Wrap(IDirect3DPixelShader9* p) { return WrapAs(p, "IDirect3DPixelShader9"); }
uint32_t Wrap(IDirect3DStateBlock9* p) { return WrapAs(p, "IDirect3DStateBlock9"); }
uint32_t Wrap(IDirect3DQuery9* p) { return WrapAs(p, "IDirect3DQuery9"); }
uint32_t Wrap(IDirect3DBaseTexture9* p) {
    if (!p) return 0;
    switch (p->GetType()) {
        case D3DRTYPE_CUBETEXTURE: return WrapAs(p, "IDirect3DCubeTexture9");
        case D3DRTYPE_VOLUMETEXTURE: return WrapAs(p, "IDirect3DVolumeTexture9");
        default: return WrapAs(p, "IDirect3DTexture9");
    }
}
uint32_t Wrap(IDirect3DResource9* p) {
    if (!p) return 0;
    switch (p->GetType()) {
        case D3DRTYPE_SURFACE: return WrapAs(p, "IDirect3DSurface9");
        case D3DRTYPE_VOLUME: return WrapAs(p, "IDirect3DVolume9");
        case D3DRTYPE_VERTEXBUFFER: return WrapAs(p, "IDirect3DVertexBuffer9");
        case D3DRTYPE_INDEXBUFFER: return WrapAs(p, "IDirect3DIndexBuffer9");
        default: return Wrap(static_cast<IDirect3DBaseTexture9*>(p));
    }
}

HWND HostHwnd(uint32_t guest_hwnd) { return guest_hwnd ? reinterpret_cast<HWND>(g_sdl_window) : nullptr; }

void ReturnFStd(Ctx* c, float v, int args) {
    c->st_top = (c->st_top - 1) & 7;
    c->st[c->st_top] = v;
    c->ret_target = Pop32(c);
    c->esp += 4 * args;
}

void ForgetResource(void* host);  // lock shadows and texture adaptation state of a released object

// Vertex and index buffers whose last reference the engine dropped live on for a few frames: the engine sometimes
// still locks a buffer right after releasing it, which on Windows works (the object outlives its last public
// reference while the device uses it) but would hit the bridge's null vtable here. The bridge keeps the engine's
// last reference and drops it at a later Present (or before Reset, which needs default-pool buffers gone).
struct Grave {
    uint32_t guest;
    IUnknown* host;
    uint32_t frame;
};
std::vector<Grave> g_graves;
uint32_t g_present_count = 0;
}  // namespace
std::atomic<uint64_t> g_frames{0};  // presented frames (watchdog, perf log)
extern std::atomic<float> g_fps;  // main.cpp: frames per second over the last second
namespace {
constexpr uint32_t kGraveFrames = 3;

ULONG RefCount(IUnknown* p) {
    p->AddRef();
    return p->Release();
}

void DrainGraves(bool all) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    size_t kept = 0;
    for (const Grave& gr : g_graves) {
        if (!all && g_present_count - gr.frame < kGraveFrames) {
            g_graves[kept++] = gr;
            continue;
        }
        if (RefCount(gr.host) == 1) {
            ForgetResource(gr.host);
            Forget(gr.guest);
        }
        gr.host->Release();  // revived by the engine meanwhile: its own references keep it
    }
    g_graves.resize(kept);
}

template <typename T>
void ReleaseImpl(Ctx* c) {
    uint32_t g = Arg(c, 0);
    T* host = Host<T>(g);
    if constexpr (std::is_same_v<T, IDirect3DVertexBuffer9> || std::is_same_v<T, IDirect3DIndexBuffer9>) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        for (const Grave& gr : g_graves) {
            if (gr.host == host && RefCount(host) == 1) {  // released again while already pending
                Return(c, 0, 1);
                return;
            }
        }
        if (RefCount(host) == 1) {
            g_graves.push_back({g, host, g_present_count});
            Return(c, 0, 1);
            return;
        }
    }
    ULONG r = host->Release();
    if (r == 0) {
        ForgetResource(host);
        Forget(g);
    }
    Return(c, r, 1);
}

template <typename T>
void QueryInterfaceImpl(Ctx* c) {
    uint32_t g = Arg(c, 0), out = Arg(c, 2);
    const GUID& riid = *Ptr<GUID>(Arg(c, 1));
    if (riid == __uuidof(IUnknown) || riid == __uuidof(T)) {
        Host<T>(g)->AddRef();
        W32(out, g);
        Return(c, S_OK, 3);
        return;
    }
    Log("d3d9: QueryInterface for an unsupported interface");
    W32(out, 0);
    Return(c, E_NOINTERFACE, 3);
}

// ---- present parameters: 56 bytes in guest memory, hDeviceWindow at +28 ----
D3DPRESENT_PARAMETERS ToHost(uint32_t g) {
    D3DPRESENT_PARAMETERS p = {};
    p.BackBufferWidth = R32(g + 0);
    p.BackBufferHeight = R32(g + 4);
    p.BackBufferFormat = D3DFORMAT(R32(g + 8));
    p.BackBufferCount = R32(g + 12);
    p.MultiSampleType = D3DMULTISAMPLE_TYPE(R32(g + 16));
    p.MultiSampleQuality = R32(g + 20);
    p.SwapEffect = D3DSWAPEFFECT(R32(g + 24));
    p.hDeviceWindow = HostHwnd(R32(g + 28) ? R32(g + 28) : HostWindow());
    p.Windowed = R32(g + 32);
    p.EnableAutoDepthStencil = R32(g + 36);
    p.AutoDepthStencilFormat = D3DFORMAT(R32(g + 40));
    p.Flags = R32(g + 44);
    p.FullScreen_RefreshRateInHz = R32(g + 48);
    p.PresentationInterval = R32(g + 52);
    // Android: the SDL window already covers the screen and exclusive fullscreen has no meaning there.
#ifdef __ANDROID__
    const bool windowed = true;
#else
    const bool windowed = getenv("SPORERE_WINDOWED") != nullptr;
#endif
    if (windowed) {
        p.Windowed = TRUE;
        p.FullScreen_RefreshRateInHz = 0;
    }
    return p;
}

void ToGuest(const D3DPRESENT_PARAMETERS& p, uint32_t g) {
    W32(g + 0, p.BackBufferWidth);
    W32(g + 4, p.BackBufferHeight);
    W32(g + 8, p.BackBufferFormat);
    W32(g + 12, p.BackBufferCount);
    W32(g + 16, p.MultiSampleType);
    W32(g + 20, p.MultiSampleQuality);
    W32(g + 24, p.SwapEffect);
    W32(g + 28, HostWindow());
    W32(g + 36, p.EnableAutoDepthStencil);
    W32(g + 40, p.AutoDepthStencilFormat);
    W32(g + 44, p.Flags);
    W32(g + 52, p.PresentationInterval);
}

// ---- lock shadows ----
struct Shadow {
    uint32_t guest = 0, capacity = 0, size = 0;
    uint32_t offset = 0;  // buffers: where in the mirror (and the buffer) the current lock starts
    void* host = nullptr;
    bool write_back = false;
    bool buffer = false;  // a vertex/index buffer mirror (capacity = the buffer's size)
    bool index = false;   // an index buffer (the game's locks of those come too short)
    uint32_t flags = 0;
    bool mirror = false;  // texture level: the shadow's first `size` bytes equal the level (no copy-in needed)
};
std::map<std::pair<void*, uint32_t>, Shadow> g_shadows;  // (object, subresource) -> shadow

bool IsBlockCompressed(D3DFORMAT f) {
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5 ||
           f == D3DFORMAT(MAKEFOURCC('A', 'T', 'I', '1')) || f == D3DFORMAT(MAKEFOURCC('A', 'T', 'I', '2'));
}

// Maps the host's locked memory into a guest buffer: copies it in unless the lock discards or never overwrites
// (the guest then only writes), and remembers to copy it back on unlock unless read-only.
// `reach`: how far the game may write from the returned pointer without harm on Windows, where it points into the
// resource itself (buffers: to the end of the buffer, even past the locked range). The shadow covers all of it, so
// such writes land in it instead of the guest heap next to it; only the locked `size` is copied back.
// `whole_level`: a lock of a whole texture level that only the game writes (no render target). After the first such
// lock the shadow holds exactly the level's bytes, so later locks skip copying the level in from GPU memory (the
// game rewrites textures while stages load; that copy was most of the loading time on a phone). Any other kind of
// lock, or a lock through one of the texture's surfaces (InvalidateMirrors), makes the next one copy again.
uint32_t ShadowLock(void* object, uint32_t sub, void* host, uint32_t size, DWORD flags, uint32_t reach = 0,
                    bool whole_level = false) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Shadow& s = g_shadows[{object, sub}];
    uint32_t need = reach > size ? reach : size;
    if (s.capacity < need) {
        if (s.guest) HeapFree(s.guest);
        s.guest = HeapAlloc(need);
        s.capacity = need;
        s.mirror = false;
    }
    bool in_sync = whole_level && s.mirror && s.size == size;
    s.size = size;
    s.host = host;
    s.write_back = !(flags & D3DLOCK_READONLY);
    if (!(flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) && !in_sync) memcpy(Ptr<void>(s.guest), host, size);
    s.mirror = whole_level;
    return s.guest;
}

// The texture behind `surface` was (or may be) written another way: its levels' shadows are no longer mirrors.
void InvalidateMirrors(IDirect3DSurface9* surface) {
    IDirect3DBaseTexture9* tex = nullptr;
    if (FAILED(surface->GetContainer(__uuidof(IDirect3DBaseTexture9), reinterpret_cast<void**>(&tex))) || !tex) return;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        for (auto it = g_shadows.lower_bound({tex, 0}); it != g_shadows.end() && it->first.first == tex; ++it)
            it->second.mirror = false;
    }
    tex->Release();
}

void ShadowUnlock(void* object, uint32_t sub) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto it = g_shadows.find({object, sub});
    if (it == g_shadows.end() || !it->second.host) return;
    Shadow& s = it->second;
    if (s.write_back) memcpy(s.host, Ptr<void>(s.guest + s.offset), s.size);
    if (s.write_back && s.buffer && s.index) {
        // The game writes past the index ranges it locks: it passes index counts where Lock takes bytes (half of
        // every 16-bit index list was missing; planet height maps were built from broken triangles, the huge spiky
        // mountains). On Windows the pointer goes into the buffer itself, so those writes count. Index locks upload
        // four times the locked size (32-bit indices), up to the end of the buffer. No comparison with the buffer:
        // reading it back was slow (on a phone GPU-visible memory is uncached; it took half the CPU time in a stage).
        const uint8_t* m = Ptr<uint8_t>(s.guest + s.offset);
        uint8_t* h = static_cast<uint8_t*>(s.host);
        uint32_t rest = s.capacity - s.offset;
        uint32_t end = s.size <= rest / 4 ? s.size * 4 : rest;
        if (end > s.size) memcpy(h + s.size, m + s.size, end - s.size);
        static const bool trace = SettingInt("trace_lock_overrun", 0) != 0;
        static std::atomic<int> n{0};
        if (trace && end > s.size && n.fetch_add(1) < 400)
            Log("lock overrun: buffer %u bytes, flags %x, lock at %u size %u, written to +%u", s.capacity, s.flags,
                s.offset, s.size, end);
    }
    s.host = nullptr;
    s.offset = 0;
}

// Vertex and index buffers: the shadow mirrors the whole buffer and a lock at `offset` returns the mirror at that
// offset. All the game's writes go through the mirror, so bytes a lock leaves untouched still hold what the game put
// there earlier, as in the real buffer on Windows. (A shadow that started at each lock's offset handed the game stale
// bytes from an earlier lock elsewhere in the buffer, and Unlock copied them over live vertices: dynamic geometry such
// as the stamps that build planet height maps came out as long spikes.) Only the locked range is copied back.
uint32_t BufferShadowLock(void* object, void* host, uint32_t total, uint32_t offset, uint32_t size, DWORD flags,
                          bool index) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Shadow& s = g_shadows[{object, 0}];
    if (s.capacity < total) {
        uint32_t g = HeapAlloc(total);
        memset(Ptr<void>(g), 0, total);
        if (s.guest) {
            memcpy(Ptr<void>(g), Ptr<void>(s.guest), s.capacity);
            HeapFree(s.guest);
        }
        s.guest = g;
        s.capacity = total;
    }
    s.offset = offset;
    s.size = size;
    s.host = host;
    s.buffer = true;
    s.index = index;
    s.flags = flags;
    s.write_back = !(flags & D3DLOCK_READONLY);
    if (flags & D3DLOCK_READONLY) memcpy(Ptr<void>(s.guest + offset), host, size);  // GPU-written (ProcessVertices)
    return s.guest + offset;
}

void ForgetShadows(void* object) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (auto it = g_shadows.lower_bound({object, 0}); it != g_shadows.end() && it->first.first == object;) {
        if (it->second.guest) HeapFree(it->second.guest);
        it = g_shadows.erase(it);
    }
}

uint32_t RectRows(const D3DSURFACE_DESC& d, const RECT* r) {
    uint32_t h = r ? uint32_t(r->bottom - r->top) : d.Height;
    return IsBlockCompressed(d.Format) ? (h + 3) / 4 : h;
}

// LockRect common part: D3DLOCKED_RECT in guest memory is {INT Pitch; void* pBits} (8 bytes).
void LockRectCommon(Ctx* c, void* object, uint32_t sub, HRESULT hr, const D3DLOCKED_RECT& lr,
                    const D3DSURFACE_DESC& desc, const RECT* rect, DWORD flags, uint32_t out, int args,
                    bool texture_level = false) {
    if (SUCCEEDED(hr)) {
        uint32_t size = uint32_t(lr.Pitch) * RectRows(desc, rect);
        bool whole = texture_level && !rect && !(desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL));
        W32(out, uint32_t(lr.Pitch));
        W32(out + 4, ShadowLock(object, sub, lr.pBits, size, flags, 0, whole));
    }
    Return(c, uint32_t(hr), args);
}

// ---- texture adaptation for mobile GPUs ----
// Invisible to the engine, which keeps seeing the textures it asked for:
//   - mip skipping (setting texture_skip_mips): a large mipmapped texture is created `skip` levels smaller; the
//     guest's top levels go to a scratch buffer and are dropped. Halves texture resolution per level, quarters
//     memory and bandwidth.
//   - DXT decoding (setting decode_dxt, automatic when the GPU has no BC formats, e.g. Mali): DXT1/3/5 textures are
//     created as A8R8G8B8 and decoded on unlock.
struct TexAdapt {
    D3DFORMAT format;   // what the guest asked for
    UINT width, height;
    UINT skip;          // guest level L is host level L - skip; levels below skip are dropped
    bool decode;        // host texture is decode_format, guest writes DXT blocks
    D3DFORMAT decode_format;  // A8R8G8B8, or A1R5G5B5 for DXT1 (half the memory, keeps its 1-bit alpha)
};
std::unordered_map<void*, TexAdapt> g_adapt;  // host texture -> adaptation, only for adapted textures
int g_skip_mips = -1;                          // resolved on first texture
UINT g_skip_min_size = 512;                    // only textures at least this large (larger side) lose levels
bool g_decode_dxt = false;
bool g_decode_16bit = false;  // decode_16bit: DXT1 -> A1R5G5B5 (Android default when the GPU samples it)

bool IsDxt(D3DFORMAT f) {
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}

// Bytes per 4x4 block (DXT) or per pixel (plain formats the adaptation handles); 0 = not adaptable.
UINT BlockBytes(D3DFORMAT f) {
    switch (f) {
        case D3DFMT_DXT1: return 8;
        case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return 16;
        case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: return 4;
        default: return 0;
    }
}

// Pitch and row count of a guest level (or of `rect` within it) in the guest's format.
void GuestLevelLayout(D3DFORMAT f, UINT w, UINT h, const RECT* rect, UINT* pitch, UINT* rows) {
    UINT rh = rect ? UINT(rect->bottom - rect->top) : h;
    if (IsDxt(f)) {
        *pitch = ((w + 3) / 4) * BlockBytes(f);
        *rows = (rh + 3) / 4;
    } else {
        *pitch = w * BlockBytes(f);
        *rows = rh;
    }
}

const TexAdapt* AdaptOf(void* host) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto it = g_adapt.find(host);
    return it == g_adapt.end() ? nullptr : &it->second;
}

// Decides how a texture of the guest's description is created on the host.
TexAdapt PlanTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format) {
    if (g_skip_mips < 0) {
#ifdef __ANDROID__
        g_skip_mips = SettingInt("texture_skip_mips", 1);
#else
        g_skip_mips = SettingInt("texture_skip_mips", 0);
#endif
        g_skip_min_size = UINT(SettingInt("texture_skip_min_size", 512));
        Log("textures: skip %d mip level(s) of textures >= %u, decode DXT %s", g_skip_mips, g_skip_min_size,
            g_decode_dxt ? "on" : "off");
    }
    TexAdapt a{format, w, h, 0, false, D3DFMT_A8R8G8B8};
    bool plain = !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC));
    if (plain && BlockBytes(format) && levels != 1 && (w > h ? w : h) >= g_skip_min_size) {
        UINT skip = UINT(g_skip_mips);
        // Keep at least one level and 4x4 blocks on the host.
        while (skip && ((levels && skip >= levels) || (w >> skip) < 4 || (h >> skip) < 4)) --skip;
        a.skip = skip;
    }
    a.decode = plain && g_decode_dxt && IsDxt(format);
    if (a.decode && g_decode_16bit && format == D3DFMT_DXT1) a.decode_format = D3DFMT_A1R5G5B5;
    // Statistics: GPU memory of the top level as the guest asked for it and as created.
    static uint64_t count = 0, reduced = 0, decoded = 0, guest_bytes = 0, host_bytes = 0;
    UINT bb = BlockBytes(format);
    if (bb) {
        uint64_t px = uint64_t(w) * h;
        uint64_t g = IsDxt(format) ? px * bb / 16 : px * bb;
        uint64_t hpx = px >> (2 * a.skip);
        guest_bytes += g;
        host_bytes += a.decode ? hpx * (a.decode_format == D3DFMT_A1R5G5B5 ? 2 : 4) : g >> (2 * a.skip);
    }
    ++count;
    reduced += a.skip != 0;
    decoded += a.decode;
    if ((count & 1023) == 0) {
        Log("textures: %llu created, %llu reduced, %llu decoded; top levels %llu MiB -> %llu MiB",
            (unsigned long long)count, (unsigned long long)reduced, (unsigned long long)decoded,
            (unsigned long long)(guest_bytes >> 20), (unsigned long long)(host_bytes >> 20));
    }
    return a;
}

// ---- DXT decoding into A8R8G8B8 (memory order B, G, R, A) ----
void Rgb565(uint16_t c, uint8_t out[4]) {
    out[2] = uint8_t(((c >> 11) & 31) * 255 / 31);
    out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    out[0] = uint8_t((c & 31) * 255 / 31);
    out[3] = 255;
}

// Colour block (DXT1 semantics when `dxt1`, else always four colours) -> 16 BGRA pixels.
void DecodeColorBlock(const uint8_t* b, bool dxt1, uint8_t px[16][4]) {
    uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t pal[4][4];
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    if (c0 > c1 || !dxt1) {
        for (int k = 0; k < 4; ++k) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k] + 1) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k] + 1) / 3);
        }
    } else {
        for (int k = 0; k < 4; ++k) pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;  // transparent black
    }
    uint32_t idx = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; ++i) memcpy(px[i], pal[(idx >> (2 * i)) & 3], 4);
}

void DecodeBlock(D3DFORMAT f, const uint8_t* b, uint8_t px[16][4]) {
    if (f == D3DFMT_DXT1) {
        DecodeColorBlock(b, true, px);
    } else if (f == D3DFMT_DXT2 || f == D3DFMT_DXT3) {
        DecodeColorBlock(b + 8, false, px);
        for (int i = 0; i < 16; ++i) px[i][3] = uint8_t(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
    } else {  // DXT4/5: interpolated alpha
        DecodeColorBlock(b + 8, false, px);
        uint8_t a[8] = {b[0], b[1]};
        if (a[0] > a[1]) {
            for (int i = 1; i < 7; ++i) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1] + 3) / 7);
        } else {
            for (int i = 1; i < 5; ++i) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1] + 2) / 5);
            a[6] = 0;
            a[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
        for (int i = 0; i < 16; ++i) px[i][3] = a[(bits >> (3 * i)) & 7];
    }
}

// Guest DXT data (`rows` block rows of `pitch` bytes) -> host BGRA pixels of a w x h region.
void DecodeDxt(D3DFORMAT f, const uint8_t* src, UINT pitch, UINT w, UINT h, uint8_t* dst, INT dst_pitch,
               D3DFORMAT dst_format) {
    const bool packed16 = dst_format == D3DFMT_A1R5G5B5;
    UINT bw = (w + 3) / 4, bh = (h + 3) / 4, bytes = BlockBytes(f);
    uint8_t px[16][4];
    for (UINT by = 0; by < bh; ++by) {
        for (UINT bx = 0; bx < bw; ++bx) {
            DecodeBlock(f, src + by * pitch + bx * bytes, px);
            for (UINT y = 0; y < 4 && by * 4 + y < h; ++y) {
                for (UINT x = 0; x < 4 && bx * 4 + x < w; ++x) {
                    const uint8_t* p = px[y * 4 + x];
                    if (packed16) {  // B, G, R, A -> A1R5G5B5
                        uint16_t v = uint16_t((p[3] >= 128 ? 0x8000 : 0) | (p[2] >> 3) << 10 | (p[1] >> 3) << 5 |
                                              (p[0] >> 3));
                        memcpy(dst + INT(by * 4 + y) * dst_pitch + (bx * 4 + x) * 2, &v, 2);
                    } else {
                        memcpy(dst + INT(by * 4 + y) * dst_pitch + (bx * 4 + x) * 4, p, 4);
                    }
                }
            }
        }
    }
}

// ---- locks of adapted textures ----
struct AdaptedLock {
    uint32_t guest = 0, capacity = 0;  // guest buffer in the guest format
    UINT pitch = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN, decode_format = D3DFMT_A8R8G8B8;
    UINT width = 0, height = 0;         // region being written
    uint8_t* host = nullptr;            // locked host memory (decode target), null for dropped levels
    INT host_pitch = 0;
};
std::map<std::pair<void*, uint32_t>, AdaptedLock> g_adapted_locks;

// Locks guest level `level` of an adapted texture whose host level is locked through `lock_host`
// (HRESULT(UINT host_level, D3DLOCKED_RECT*)); writes D3DLOCKED_RECT to the guest.
template <typename LockHost>
HRESULT AdaptedLockRect(void* tex, uint32_t sub, const TexAdapt& a, UINT level, const RECT* rect, DWORD flags,
                        uint32_t out, LockHost lock_host) {
    UINT w = a.width >> level ? a.width >> level : 1, h = a.height >> level ? a.height >> level : 1;
    UINT pitch, rows;
    GuestLevelLayout(a.format, w, h, rect, &pitch, &rows);
    D3DLOCKED_RECT lr{};
    bool dropped = level < a.skip;
    if (!dropped) {
        HRESULT hr = lock_host(level - a.skip, &lr);
        if (FAILED(hr)) return hr;
        if (!a.decode) {  // same format and size on the host: plain shadow
            W32(out, uint32_t(lr.Pitch));
            W32(out + 4, ShadowLock(tex, sub, lr.pBits, uint32_t(lr.Pitch) * rows, flags, 0, !rect));
            return D3D_OK;
        }
    }
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    AdaptedLock& l = g_adapted_locks[{tex, sub}];
    uint32_t size = pitch * rows;
    if (l.capacity < size) {
        if (l.guest) HeapFree(l.guest);
        l.guest = HeapAlloc(size);
        l.capacity = size;
    }
    l.pitch = pitch;
    l.format = a.format;
    l.decode_format = a.decode_format;
    l.width = rect ? UINT(rect->right - rect->left) : w;
    l.height = rect ? UINT(rect->bottom - rect->top) : h;
    l.host = dropped ? nullptr : static_cast<uint8_t*>(lr.pBits);
    l.host_pitch = lr.Pitch;
    W32(out, pitch);
    W32(out + 4, l.guest);
    return D3D_OK;
}

// Returns true when the lock was a decoded or dropped one (the host level, if any, must still be unlocked).
bool AdaptedUnlock(void* tex, uint32_t sub, bool* host_locked) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto it = g_adapted_locks.find({tex, sub});
    if (it == g_adapted_locks.end() || !it->second.guest) return false;
    AdaptedLock& l = it->second;
    *host_locked = l.host != nullptr;
    if (l.host) {
        DecodeDxt(l.format, Ptr<uint8_t>(l.guest), l.pitch, l.width, l.height, l.host, l.host_pitch, l.decode_format);
        l.host = nullptr;
    }
    return true;
}

void ForgetResource(void* host) {
    ForgetShadows(host);
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_adapt.erase(host);
    for (auto it = g_adapted_locks.lower_bound({host, 0}); it != g_adapted_locks.end() && it->first.first == host;) {
        if (it->second.guest) HeapFree(it->second.guest);
        it = g_adapted_locks.erase(it);
    }
}

// Guest view of a level description: guest size and format.
void AdaptDesc(const TexAdapt& a, UINT level, D3DSURFACE_DESC* d) {
    d->Format = a.format;
    d->Width = a.width >> level ? a.width >> level : 1;
    d->Height = a.height >> level ? a.height >> level : 1;
}


// ---- display modes ----
// With override_modes (default on Android) the engine sees modes of the render resolution instead of the screen's:
// the render size (render_height) first, then 540p/720p/900p/1080p at the screen's aspect ratio. Its default
// fullscreen choice, the back buffer and the window client area then agree, and touch coordinates map 1:1.
bool OverrideModes() {
#ifdef __ANDROID__
    static const bool on = SettingInt("override_modes", 1) != 0;
#else
    static const bool on = SettingInt("override_modes", 0) != 0;
#endif
    return on;
}

std::vector<D3DDISPLAYMODE> Modes() {
    static std::vector<D3DDISPLAYMODE> modes;
    if (modes.empty()) {
        int cw, ch;
        ClientSize(&cw, &ch);
        modes.push_back({UINT(cw), UINT(ch), 60, D3DFMT_X8R8G8B8});
        for (UINT h : {540u, 720u, 900u, 1080u}) {
            UINT w = (UINT(uint64_t(h) * UINT(cw) / UINT(ch)) + 4) & ~7u;
            if (h != UINT(ch)) modes.push_back({w, h, 60, D3DFMT_X8R8G8B8});
        }
    }
    return modes;
}

void WriteMode(uint32_t out, const D3DDISPLAYMODE& m) {
    W32(out, m.Width);
    W32(out + 4, m.Height);
    W32(out + 8, m.RefreshRate);
    W32(out + 12, uint32_t(m.Format));
}

// ---- Direct3DCreate9 ----
void x_Direct3DCreate9(Ctx* c) {
    IDirect3D9* d3d = Direct3DCreate9(Arg(c, 0));
    Log("Direct3DCreate9(%u) -> %p", Arg(c, 0), static_cast<void*>(d3d));
    g_trace_draws = SettingInt("trace_hm_draws", 0);
    // GPUs without BC formats (Mali, PowerVR) get DXT textures decoded on the CPU; decode_dxt=1/0 forces it.
    if (d3d) {
        bool bc = SUCCEEDED(d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE,
                                                   D3DFMT_DXT5));
        g_decode_dxt = SettingInt("decode_dxt", bc ? 0 : 1) != 0;
        bool a1r5 = SUCCEEDED(d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE,
                                                     D3DFMT_A1R5G5B5));
#ifdef __ANDROID__
        g_decode_16bit = a1r5 && SettingInt("decode_16bit", 1) != 0;
#else
        g_decode_16bit = a1r5 && SettingInt("decode_16bit", 0) != 0;
#endif
        Log("GPU BC (DXT) textures: %s, decoding %s%s", bc ? "supported" : "not supported", g_decode_dxt ? "on" : "off",
            g_decode_dxt && g_decode_16bit ? " (DXT1 as 16-bit)" : "");
    }
    Return(c, Wrap(d3d), 1);
}

}  // namespace

// ---------------- manual methods ----------------
#define M(iface, method) void Manual_##iface##_##method(Ctx* c)

M(IDirect3D9, GetAdapterMonitor) { Return(c, 1, 2); }
// GetRenderTargetData: GPU results the game reads back (planet height maps among them). dump_rtd = N writes the first
// N of them as rtd_<n>_<w>x<h>_<format>.raw (rows packed, no padding) into the working directory, for debugging.
M(IDirect3DDevice9, GetRenderTargetData) {
    auto* src = Unwrap<IDirect3DSurface9>(Arg(c, 1));
    auto* dst = Unwrap<IDirect3DSurface9>(Arg(c, 2));
    HRESULT r = Host<IDirect3DDevice9>(Arg(c, 0))->GetRenderTargetData(src, dst);
    static const int limit = SettingInt("dump_rtd", 0);
    static std::atomic<int> n{0};
    D3DSURFACE_DESC d;
    if (SUCCEEDED(r) && limit > 0 && SUCCEEDED(dst->GetDesc(&d))) {
        int k = n.fetch_add(1);
        D3DLOCKED_RECT lr;
        if (k < limit && SUCCEEDED(dst->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            uint32_t bpp = d.Format == D3DFMT_A32B32G32R32F ? 16 : d.Format == D3DFMT_A16B16G16R16F ? 8
                           : d.Format == D3DFMT_G32R32F ? 8 : d.Format == D3DFMT_R16F ? 2 : 4;
            char name[96];
            snprintf(name, sizeof name, "rtd_%03d_%ux%u_%u.raw", k, d.Width, d.Height, unsigned(d.Format));
            if (FILE* f = fopen(name, "wb")) {
                for (uint32_t y = 0; y < d.Height; ++y)
                    fwrite(static_cast<const uint8_t*>(lr.pBits) + size_t(y) * size_t(lr.Pitch), 1, d.Width * bpp, f);
                fclose(f);
            }
            dst->UnlockRect();
            Log("rtd %d: %ux%u format %u pitch %d", k, d.Width, d.Height, unsigned(d.Format), lr.Pitch);
        }
    }
    Return(c, uint32_t(r), 3);
}
M(IDirect3D9, GetAdapterDisplayMode) {
    // (this, Adapter, pMode)
    if (!OverrideModes()) {
        Return(c, uint32_t(Host<IDirect3D9>(Arg(c, 0))->GetAdapterDisplayMode(Arg(c, 1), Ptr<D3DDISPLAYMODE>(Arg(c, 2)))),
               3);
        return;
    }
    WriteMode(Arg(c, 2), Modes()[0]);
    Return(c, D3D_OK, 3);
}
M(IDirect3D9, GetAdapterModeCount) {
    // (this, Adapter, Format)
    if (!OverrideModes()) {
        Return(c, Host<IDirect3D9>(Arg(c, 0))->GetAdapterModeCount(Arg(c, 1), D3DFORMAT(Arg(c, 2))), 3);
        return;
    }
    D3DFORMAT f = D3DFORMAT(Arg(c, 2));
    Return(c, f == D3DFMT_X8R8G8B8 || f == D3DFMT_A8R8G8B8 ? uint32_t(Modes().size()) : 0u, 3);
}
M(IDirect3D9, EnumAdapterModes) {
    // (this, Adapter, Format, Mode, pMode)
    if (!OverrideModes()) {
        Return(c, uint32_t(Host<IDirect3D9>(Arg(c, 0))->EnumAdapterModes(Arg(c, 1), D3DFORMAT(Arg(c, 2)), Arg(c, 3),
                                                                        Ptr<D3DDISPLAYMODE>(Arg(c, 4)))),
               5);
        return;
    }
    auto modes = Modes();
    if (Arg(c, 3) >= modes.size()) {
        Return(c, uint32_t(D3DERR_INVALIDCALL), 5);
        return;
    }
    D3DDISPLAYMODE m = modes[Arg(c, 3)];
    m.Format = D3DFORMAT(Arg(c, 2));
    WriteMode(Arg(c, 4), m);
    Return(c, D3D_OK, 5);
}
M(IDirect3DDevice9, GetDisplayMode) {
    // (this, iSwapChain, pMode)
    if (!OverrideModes()) {
        Return(c, uint32_t(Host<IDirect3DDevice9>(Arg(c, 0))->GetDisplayMode(Arg(c, 1),
                                                                            Ptr<D3DDISPLAYMODE>(Arg(c, 2)))),
               3);
        return;
    }
    WriteMode(Arg(c, 2), Modes()[0]);
    Return(c, D3D_OK, 3);
}

bool WaitForWindow(int seconds);  // below, with the surface state

M(IDirect3D9, CreateDevice) {
    D3DPRESENT_PARAMETERS pp = ToHost(Arg(c, 5));
    IDirect3DDevice9* dev = nullptr;
    // The Vulkan surface needs the window on screen: if the player switched to another app during startup, the
    // device would fail with a lost surface. Wait for the window, and retry a failure that happened while it was gone.
    HRESULT hr = D3DERR_INVALIDCALL;
    for (int attempt = 0; attempt < 5; ++attempt) {
        WaitForWindow(600);
        D3DPRESENT_PARAMETERS try_pp = pp;
        hr = Host<IDirect3D9>(Arg(c, 0))->CreateDevice(Arg(c, 1), D3DDEVTYPE(Arg(c, 2)), HostHwnd(Arg(c, 3)),
                                                     Arg(c, 4), &try_pp, &dev);
        if (SUCCEEDED(hr)) {
            pp = try_pp;
            break;
        }
        if (!WaitForWindow(0)) {
            Log("CreateDevice -> %08x while the window was away, retrying", uint32_t(hr));
            continue;
        }
        break;  // failed with the window on screen: a real driver problem
    }
    Log("CreateDevice %ux%u fmt %u windowed %u msaa %u/%u interval %u -> %08x", pp.BackBufferWidth,
        pp.BackBufferHeight, pp.BackBufferFormat, pp.Windowed, pp.MultiSampleType, pp.MultiSampleQuality,
        pp.PresentationInterval, uint32_t(hr));
    // The engine does not check: without a device it crashes on the next call. The DXVK log says why it failed.
    if (FAILED(hr)) {
        StartupFailure("The graphics driver could not create the D3D9 device (DXVK on Vulkan). "
                       "See app_process64_d3d9.log for the reason.");
    }
    if (OverrideModes()) SetClientSize(int(pp.BackBufferWidth), int(pp.BackBufferHeight));
    ToGuest(pp, Arg(c, 5));
    W32(Arg(c, 6), Wrap(dev));
    Return(c, uint32_t(hr), 7);
}
M(IDirect3DDevice9, GetCreationParameters) {
    D3DDEVICE_CREATION_PARAMETERS p;
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->GetCreationParameters(&p);
    uint32_t g = Arg(c, 1);
    W32(g, p.AdapterOrdinal);
    W32(g + 4, p.DeviceType);
    W32(g + 8, HostWindow());
    W32(g + 12, p.BehaviorFlags);
    Return(c, uint32_t(hr), 2);
}
M(IDirect3DDevice9, SetCursorProperties) {
    Return(c, uint32_t(Host<IDirect3DDevice9>(Arg(c, 0))->SetCursorProperties(
                  Arg(c, 1), Arg(c, 2), Unwrap<IDirect3DSurface9>(Arg(c, 3)))),
           4);
}
M(IDirect3DDevice9, CreateAdditionalSwapChain) {
    D3DPRESENT_PARAMETERS pp = ToHost(Arg(c, 1));
    IDirect3DSwapChain9* sc = nullptr;
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->CreateAdditionalSwapChain(&pp, &sc);
    ToGuest(pp, Arg(c, 1));
    W32(Arg(c, 2), SUCCEEDED(hr) ? Wrap(sc) : 0);
    Return(c, uint32_t(hr), 3);
}
M(IDirect3DDevice9, Reset) {
    D3DPRESENT_PARAMETERS pp = ToHost(Arg(c, 1));
    DrainGraves(true);
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->Reset(&pp);
    Log("Reset %ux%u msaa %u/%u interval %u -> %08x", pp.BackBufferWidth, pp.BackBufferHeight, pp.MultiSampleType,
        pp.MultiSampleQuality, pp.PresentationInterval, uint32_t(hr));
    if (SUCCEEDED(hr) && OverrideModes()) SetClientSize(int(pp.BackBufferWidth), int(pp.BackBufferHeight));
    ToGuest(pp, Arg(c, 1));
    Return(c, uint32_t(hr), 2);
}
// Setting shot=<frame>[,<frame>...] (SPORERE_SHOT): the back buffer of those presents is saved as shot_<frame>.ppm in
// the working directory (debugging without a desktop screenshot tool; on Android only the game's own frame).
void MaybeScreenshot(IDirect3DDevice9* dev) {
    static const std::string spec_setting = Setting("shot", "");  // sporere.ini `shot = ...` works on Android
    static const char* spec = spec_setting.empty() ? nullptr : spec_setting.c_str();
    static uint32_t frame = 0;
    ++frame;
    // On demand: a file named shot_now in the working directory (adb shell touch ...) asks for one frame.
    bool now = (frame & 15) == 0 && access("shot_now", F_OK) == 0;
    if (now) unlink("shot_now");
    if (!spec && !now) return;
    bool want = now;
    for (const char* p = spec ? spec : ""; *p;) {
        char* end;
        if (strtoul(p, &end, 10) == frame) want = true;
        p = *end ? end + 1 : end;
    }
    if (!want) return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    IDirect3DSurface9* sys = nullptr;
    D3DLOCKED_RECT lr;
    if ((d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) &&
        SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(bb, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        std::string name = now ? std::string("shot_now.ppm") : "shot_" + std::to_string(frame) + ".ppm";
        if (FILE* f = fopen(name.c_str(), "wb")) {
            fprintf(f, "P6\n%u %u\n255\n", d.Width, d.Height);
            std::vector<uint8_t> row(3 * d.Width);
            for (uint32_t y = 0; y < d.Height; ++y) {
                const uint8_t* src = static_cast<const uint8_t*>(lr.pBits) + size_t(y) * size_t(lr.Pitch);
                for (uint32_t x = 0; x < d.Width; ++x) {
                    row[3 * x] = src[4 * x + 2];
                    row[3 * x + 1] = src[4 * x + 1];
                    row[3 * x + 2] = src[4 * x];
                }
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
            Log("screenshot %s (%ux%u)", name.c_str(), d.Width, d.Height);
        }
        sys->UnlockRect();
    } else {
        Log("screenshot of frame %u failed (format %d)", frame, int(d.Format));
    }
    if (sys) sys->Release();
    bb->Release();
}

// The window can go away under the render thread (Android destroys the surface when the app leaves the screen):
// SporeSurface.surfaceDestroyed waits for a present in flight and holds presents back until the surface is back,
// as presenting or recreating the swap chain on a destroyed window crashes in the Vulkan loader. A skipped present
// just drops the frame.
std::mutex g_present_mutex;
bool g_surface_gone = false;  // under g_present_mutex

template <typename F>
HRESULT GuardedPresent(F present) {
    std::unique_lock<std::mutex> lock(g_present_mutex);
    if (g_surface_gone) {
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return D3D_OK;
    }
    return present();
}

#ifdef __ANDROID__
// Waits up to `seconds` for the game's window (surface) to be on screen; true if it is.
bool WaitForWindow(int seconds) {
    for (int i = 0; i <= seconds * 10; ++i) {
        {
            std::lock_guard<std::mutex> lock(g_present_mutex);
            if (!g_surface_gone) return true;
        }
        if (i < seconds * 10) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

extern "C" JNIEXPORT void JNICALL Java_com_sporere_game_SporeControls_nativeSetFpsLimit(JNIEnv*, jclass, jint fps) {
    native::g_fps_limit.store(fps > 0 ? fps : 0, std::memory_order_relaxed);
}
extern "C" JNIEXPORT jint JNICALL Java_com_sporere_game_SporeControls_nativeGameMode(JNIEnv*, jclass) {
    return jint(native::g_game_mode.load(std::memory_order_relaxed));
}
extern "C" JNIEXPORT jfloat JNICALL Java_com_sporere_game_SporeControls_nativeFps(JNIEnv*, jclass) {
    return native::g_fps.load(std::memory_order_relaxed);
}
extern "C" JNIEXPORT void JNICALL Java_com_sporere_game_SporeSurface_nativeSurfaceGone(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_present_mutex);
    g_surface_gone = true;
}
extern "C" JNIEXPORT void JNICALL Java_com_sporere_game_SporeSurface_nativeSurfaceReady(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_present_mutex);
    g_surface_gone = false;
}
#else
// Desktop: the window stays on screen.
bool WaitForWindow(int) { return true; }
#endif

// Spore waits for the GPU at the end of a frame by calling GetData on an event query in a tight loop (millions of
// calls a second on the desktop). The answer is the same, but polling costs a whole core and battery on a phone:
// only the first miss of a streak flushes (D3DGETDATA_FLUSH makes DXVK submit), later misses yield the core and,
// for a longer wait, sleep a little.
// fake_gpu = <vendor hex>:<device hex>:<description> (SPORERE_FAKE_GPU): report another adapter to the game, to
// reproduce on the desktop what the game does with a phone GPU (its configuration scripts pick settings by vendor
// and device id).
M(IDirect3D9, GetAdapterIdentifier) {
    auto* id = reinterpret_cast<D3DADAPTER_IDENTIFIER9*>(Ptr<void>(Arg(c, 3)));
    HRESULT r = Host<IDirect3D9>(Arg(c, 0))->GetAdapterIdentifier(Arg(c, 1), Arg(c, 2), id);
    static const std::string fake = Setting("fake_gpu", "");
    if (SUCCEEDED(r) && !fake.empty()) {
        unsigned vendor = 0, device = 0;
        char desc[256] = {0};
        if (sscanf(fake.c_str(), "%x:%x:%255[^\n]", &vendor, &device, desc) >= 2) {
            id->VendorId = vendor;
            id->DeviceId = device;
            if (desc[0]) snprintf(id->Description, sizeof id->Description, "%s", desc);
        }
    }
    // The game's configuration scripts only know AMD, NVIDIA, Intel and S3 adapters (Config/VideoCards.txt): for any
    // other vendor (phone GPUs: ARM Mali, Qualcomm Adreno, Imagination) the script stops with "Unknown variable:
    // 'isCardFound'". Such adapters are reported as a mid-range card from that table, a Radeon HD 2600, so the game
    // finds a card and picks moderate settings.
    if (SUCCEEDED(r)) {
        switch (id->VendorId) {
            case 0x1002: case 0x10de: case 0x10b4: case 0x12d2: case 0x8086: case 0x5333: break;
            default: {
                // The card the game sees picks its graphics settings. Setting gpu_tier (the settings panel's "Game
                // graphics"): 0 = Radeon X1300 (low, default: phones), 1 = Radeon HD 2600 (medium), 2 = Radeon HD 4850 (high).
                struct Card { uint32_t device; const char* name; };
                static const Card kTiers[] = {
                    {0x7142, "ATI Radeon X1300"}, {0x9588, "ATI Radeon HD 2600"}, {0x9442, "ATI Radeon HD 4850"}};
                // ARM Mali gets the NVIDIA cards of the same tiers: on Mali-G720 the game's ATI shadow path left the
                // ground and shadows far too dark, the NVIDIA one renders right (issue #13)
                static const Card kNvTiers[] = {
                    {0x01df, "NVIDIA GeForce 7300 GS"}, {0x0402, "NVIDIA GeForce 8600 GT"}, {0x0611, "NVIDIA GeForce 8800 GT"}};
                const bool nv = id->VendorId == 0x13b5;
                int tier = SettingInt("gpu_tier", 0);
                if (tier < 0 || tier > 2) tier = 1;
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    Log("d3d9: adapter \"%s\" (vendor %04x) is unknown to the game; reported as %s",
                        id->Description, unsigned(id->VendorId), (nv ? kNvTiers : kTiers)[tier].name);
                }
                id->VendorId = nv ? 0x10de : 0x1002;
                id->DeviceId = (nv ? kNvTiers : kTiers)[tier].device;
                snprintf(id->Description, sizeof id->Description, "%s", (nv ? kNvTiers : kTiers)[tier].name);
            }
        }
    }
    if (SUCCEEDED(r)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("d3d9: adapter \"%s\" vendor %04x device %04x driver %s", id->Description, unsigned(id->VendorId),
                unsigned(id->DeviceId), id->Driver);
        }
    }
    Return(c, uint32_t(r), 4);
}

M(IDirect3DQuery9, GetData) {
    // Spore polls an event query at the end of every frame until the GPU has finished it: the CPU and the GPU then
    // take turns instead of working on consecutive frames at once, which on a phone (both slow) costs a lot of frame
    // rate. Event queries answer "done" at once (gpu_sync = 1 restores the wait); DXVK still bounds how many frames
    // the CPU may run ahead (its frame latency limit). Occlusion and other queries return their real results.
    static const bool gpu_sync = SettingInt("gpu_sync", 0) != 0;
    auto* q = Host<IDirect3DQuery9>(Arg(c, 0));
    if (!gpu_sync && q->GetType() == D3DQUERYTYPE_EVENT) {
        if (Arg(c, 1) && Arg(c, 2) >= 4) W32(Arg(c, 1), 1);  // BOOL TRUE: the event has passed
        Return(c, uint32_t(S_OK), 4);
        return;
    }
    static thread_local uint32_t misses = 0;
    DWORD flags = Arg(c, 3);
    if (misses > 0) flags &= ~D3DGETDATA_FLUSH;
    HRESULT r = Host<IDirect3DQuery9>(Arg(c, 0))->GetData(Ptr<void>(Arg(c, 1)), Arg(c, 2), flags);
    if (r == S_FALSE) {
        ++misses;
        if (misses > 256) std::this_thread::sleep_for(std::chrono::microseconds(100));
        else if (misses > 16) std::this_thread::yield();
    } else {
        misses = 0;
    }
    Return(c, uint32_t(r), 4);
}

// The game mode that is active (App::cGameModeManager, read like its GetActiveModeID): the touch controls switch
// their layout per stage. Cell 0x1654c00, creature ..c01, tribe ..c02, civilization ..c04, space ..c05, editors
// 0xdbdba1. Read once a frame on the game's own thread; 0 while there is none.
std::atomic<uint32_t> g_game_mode{0};
void UpdateGameMode() {
    constexpr uint32_t kGameModeManager = 0x015fd8b0;  // App::IGameModeManager::Get() reads this global
    // the manager and its mode vector are heap objects: anything outside the guest heap arena (memory.cpp) is a
    // stale or half-built manager (a read at 0x275d0 crashed a phone), so it counts as no mode
    auto heap = [](uint32_t a) { return a >= 0x02000000u && a < 0x7fff0000u; };
    uint32_t mode = 0, mgr = R32(kGameModeManager);
    if (heap(mgr)) {
        int32_t idx = int32_t(R32(mgr + 0x28));
        uint32_t begin = R32(mgr + 0x14), end = R32(mgr + 0x18);
        uint32_t count = end >= begin ? (end - begin) / 24 : 0;
        if (heap(begin) && count <= 64 && idx >= 0 && uint32_t(idx) < count) mode = R32(begin + 24 * uint32_t(idx) + 4);
    }
    if (mode != g_game_mode.exchange(mode, std::memory_order_relaxed)) Log("game mode: %08x", mode);
}

M(IDirect3DDevice9, Present) {
    ++g_present_count;
    uint64_t frame = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;  // the watchdog's frame counter (main.cpp)
    // ADPF: how long this frame's work took (from the end of the previous Present to now, before any pacing sleep).
    {
        static auto work_start = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        PerfHintFrame(std::chrono::duration_cast<std::chrono::nanoseconds>(now - work_start).count());
        work_start = now;
    }
    // fps_limit (sporere.ini, the settings panel's presets): frames are paced to at most that rate, which keeps a
    // phone cooler (and its clocks up longer) when the game would otherwise run unevenly above it.
    {
        // set from sporere.ini at the first frame, then live from the settings panel (nativeSetFpsLimit)
        static const bool init = (g_fps_limit.store(SettingInt("fps_limit", 0)), true);
        (void)init;
        const int limit = g_fps_limit.load(std::memory_order_relaxed);
        static auto next = std::chrono::steady_clock::now();
        if (limit > 0) {
            auto now = std::chrono::steady_clock::now();
            if (now < next) std::this_thread::sleep_until(next);
            next = std::max(now, next) + std::chrono::microseconds(1000000 / limit);
        }
    }
    // FPS over the last second, for the on-screen counter (SporeControls.nativeFps).
    {
        static auto window = std::chrono::steady_clock::now();
        static uint64_t window_frame = 0;
        auto now = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(now - window).count();
        if (secs >= 1.0) {
            g_fps.store(float(double(frame - window_frame) / secs), std::memory_order_relaxed);
            window = now;
            window_frame = frame;
        }
    }
    // A perf line every 10 s: frames per second and the frame count it was measured at.
    {
        static auto last = std::chrono::steady_clock::now();
        static uint64_t last_frame = 0;
        auto now = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(now - last).count();
        if (secs >= 10.0) {
            Log("perf: frame %llu, %.1f fps; io %s", (unsigned long long)frame, double(frame - last_frame) / secs,
                TakeIoStats().c_str());
            last = now;
            last_frame = frame;
        }
    }
    UpdateGameMode();
    if (!g_graves.empty()) DrainGraves(false);
    auto* dev = Host<IDirect3DDevice9>(Arg(c, 0));
    MaybeScreenshot(dev);
    HRESULT hr =
        GuardedPresent([&] { return dev->Present(Ptr<RECT>(Arg(c, 1)), Ptr<RECT>(Arg(c, 2)), nullptr, nullptr); });
    Return(c, uint32_t(hr), 5);
}
// Sampler state passes through; the first 200000 calls are tallied once to log the filtering the engine asks for
// (anisotropy and mip filtering are what a phone GPU pays for).
M(IDirect3DDevice9, SetSamplerState) {
    static std::atomic<uint32_t> calls{0};
    static std::atomic<uint32_t> aniso[17], minf[4], mipf[4];
    const DWORD type = Arg(c, 2), value = Arg(c, 3);
    if (uint32_t n = calls.fetch_add(1); n < 200000) {
        if (type == D3DSAMP_MAXANISOTROPY) ++aniso[value > 16 ? 16 : value];
        if (type == D3DSAMP_MINFILTER) ++minf[value & 3];
        if (type == D3DSAMP_MIPFILTER) ++mipf[value & 3];
        if (n == 199999) {
            std::string a;
            for (int i = 0; i <= 16; ++i)
                if (aniso[i]) a += " " + std::to_string(i) + "x:" + std::to_string(aniso[i].load());
            Log("samplers: max anisotropy%s | min filter none/point/linear/aniso %u/%u/%u/%u | mip %u/%u/%u/%u",
                a.c_str(), minf[0].load(), minf[1].load(), minf[2].load(), minf[3].load(), mipf[0].load(),
                mipf[1].load(), mipf[2].load(), mipf[3].load());
        }
    }
    Return(c, uint32_t(Host<IDirect3DDevice9>(Arg(c, 0))->SetSamplerState(Arg(c, 1), D3DSAMPLERSTATETYPE(type),
                                                                          value)), 4);
}
// The engine's worker threads poll this before every buffer lock; DXVK answers D3D_OK for a windowed device but
// takes the device lock first, so the polls queue up behind the render thread's draws and presents.
M(IDirect3DDevice9, TestCooperativeLevel) { Return(c, uint32_t(D3D_OK), 1); }
M(IDirect3DSwapChain9, Present) {
    auto* sc = Host<IDirect3DSwapChain9>(Arg(c, 0));
    HRESULT hr = GuardedPresent([&] { return sc->Present(Ptr<RECT>(Arg(c, 1)), Ptr<RECT>(Arg(c, 2)), nullptr, nullptr,
                                                         Arg(c, 5)); });
    Return(c, uint32_t(hr), 6);
}
M(IDirect3DSwapChain9, GetPresentParameters) {
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr = Host<IDirect3DSwapChain9>(Arg(c, 0))->GetPresentParameters(&pp);
    ToGuest(pp, Arg(c, 1));
    Return(c, uint32_t(hr), 2);
}

// GetContainer(riid, void**): the container is a texture or the device.
template <typename T>
void GetContainerImpl(Ctx* c) {
    const GUID& riid = *Ptr<GUID>(Arg(c, 1));
    void* p = nullptr;
    HRESULT hr = Host<T>(Arg(c, 0))->GetContainer(riid, &p);
    uint32_t g = 0;
    if (SUCCEEDED(hr)) {
        if (riid == __uuidof(IDirect3DTexture9)) g = Wrap(static_cast<IDirect3DTexture9*>(p));
        else if (riid == __uuidof(IDirect3DCubeTexture9)) g = Wrap(static_cast<IDirect3DCubeTexture9*>(p));
        else if (riid == __uuidof(IDirect3DVolumeTexture9)) g = Wrap(static_cast<IDirect3DVolumeTexture9*>(p));
        else if (riid == __uuidof(IDirect3DBaseTexture9)) g = Wrap(static_cast<IDirect3DBaseTexture9*>(p));
        else if (riid == __uuidof(IDirect3DDevice9)) g = Wrap(static_cast<IDirect3DDevice9*>(p));
        else Log("d3d9: GetContainer for an unsupported interface");
    }
    W32(Arg(c, 2), g);
    Return(c, uint32_t(hr), 3);
}
M(IDirect3DSurface9, GetContainer) { GetContainerImpl<IDirect3DSurface9>(c); }
M(IDirect3DVolume9, GetContainer) { GetContainerImpl<IDirect3DVolume9>(c); }

M(IDirect3DSurface9, LockRect) {
    auto* s = Host<IDirect3DSurface9>(Arg(c, 0));
    const RECT* rect = Ptr<RECT>(Arg(c, 2));
    D3DLOCKED_RECT lr;
    HRESULT hr = s->LockRect(&lr, rect, Arg(c, 3));
    D3DSURFACE_DESC desc;
    s->GetDesc(&desc);
    if (SUCCEEDED(hr) && !(Arg(c, 3) & D3DLOCK_READONLY)) InvalidateMirrors(s);  // a texture level written directly
    LockRectCommon(c, s, 0, hr, lr, desc, rect, Arg(c, 3), Arg(c, 1), 4);
}
M(IDirect3DSurface9, UnlockRect) {
    auto* s = Host<IDirect3DSurface9>(Arg(c, 0));
    ShadowUnlock(s, 0);
    Return(c, uint32_t(s->UnlockRect()), 1);
}
M(IDirect3DSurface9, GetDC) { Return(c, uint32_t(D3DERR_INVALIDCALL), 2); }
M(IDirect3DSurface9, ReleaseDC) { Return(c, uint32_t(D3DERR_INVALIDCALL), 2); }

M(IDirect3DDevice9, CreateTexture) {
    // (this, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle)
    UINT w = Arg(c, 1), h = Arg(c, 2), levels = Arg(c, 3);
    DWORD usage = Arg(c, 4);
    D3DFORMAT format = D3DFORMAT(Arg(c, 5));
    TexAdapt a = PlanTexture(w, h, levels, usage, format);
    IDirect3DTexture9* tex = nullptr;
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->CreateTexture(
        w >> a.skip, h >> a.skip, levels ? levels - a.skip : 0, usage, a.decode ? a.decode_format : format,
        D3DPOOL(Arg(c, 6)), &tex, nullptr);
    if (SUCCEEDED(hr) && (a.skip || a.decode)) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_adapt[tex] = a;
    }
    W32(Arg(c, 7), SUCCEEDED(hr) ? Wrap(tex) : 0);
    Return(c, uint32_t(hr), 9);
}
M(IDirect3DDevice9, CreateCubeTexture) {
    // (this, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle)
    UINT edge = Arg(c, 1), levels = Arg(c, 2);
    DWORD usage = Arg(c, 3);
    D3DFORMAT format = D3DFORMAT(Arg(c, 4));
    TexAdapt a = PlanTexture(edge, edge, levels, usage, format);
    IDirect3DCubeTexture9* tex = nullptr;
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->CreateCubeTexture(
        edge >> a.skip, levels ? levels - a.skip : 0, usage, a.decode ? a.decode_format : format, D3DPOOL(Arg(c, 5)),
        &tex, nullptr);
    if (SUCCEEDED(hr) && (a.skip || a.decode)) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_adapt[tex] = a;
    }
    W32(Arg(c, 6), SUCCEEDED(hr) ? Wrap(tex) : 0);
    Return(c, uint32_t(hr), 8);
}
M(IDirect3D9, CheckDeviceFormat) {
    // (this, Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat): DXT is always available when decoded.
    D3DFORMAT check = D3DFORMAT(Arg(c, 6));
    HRESULT hr = Host<IDirect3D9>(Arg(c, 0))->CheckDeviceFormat(Arg(c, 1), D3DDEVTYPE(Arg(c, 2)),
                                                              D3DFORMAT(Arg(c, 3)), Arg(c, 4),
                                                              D3DRESOURCETYPE(Arg(c, 5)), check);
    if (FAILED(hr) && g_decode_dxt && IsDxt(check) && !(Arg(c, 4) & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) {
        hr = D3D_OK;
    }
    Return(c, uint32_t(hr), 7);
}
M(IDirect3DTexture9, GetLevelCount) {
    auto* t = Host<IDirect3DTexture9>(Arg(c, 0));
    const TexAdapt* a = AdaptOf(t);
    Return(c, t->GetLevelCount() + (a ? a->skip : 0), 1);
}
M(IDirect3DCubeTexture9, GetLevelCount) {
    auto* t = Host<IDirect3DCubeTexture9>(Arg(c, 0));
    const TexAdapt* a = AdaptOf(t);
    Return(c, t->GetLevelCount() + (a ? a->skip : 0), 1);
}
M(IDirect3DTexture9, GetLevelDesc) {
    auto* t = Host<IDirect3DTexture9>(Arg(c, 0));
    UINT level = Arg(c, 1);
    const TexAdapt* a = AdaptOf(t);
    D3DSURFACE_DESC* d = Ptr<D3DSURFACE_DESC>(Arg(c, 2));
    HRESULT hr = t->GetLevelDesc(a && level >= a->skip ? level - a->skip : a ? 0 : level, d);
    if (SUCCEEDED(hr) && a) AdaptDesc(*a, level, d);
    Return(c, uint32_t(hr), 3);
}
M(IDirect3DCubeTexture9, GetLevelDesc) {
    auto* t = Host<IDirect3DCubeTexture9>(Arg(c, 0));
    UINT level = Arg(c, 1);
    const TexAdapt* a = AdaptOf(t);
    D3DSURFACE_DESC* d = Ptr<D3DSURFACE_DESC>(Arg(c, 2));
    HRESULT hr = t->GetLevelDesc(a && level >= a->skip ? level - a->skip : a ? 0 : level, d);
    if (SUCCEEDED(hr) && a) AdaptDesc(*a, level, d);
    Return(c, uint32_t(hr), 3);
}
// Surfaces of adapted textures: the nearest host level (dropped levels map to the top host level).
M(IDirect3DTexture9, GetSurfaceLevel) {
    auto* t = Host<IDirect3DTexture9>(Arg(c, 0));
    const TexAdapt* a = AdaptOf(t);
    UINT level = Arg(c, 1);
    IDirect3DSurface9* surf = nullptr;
    HRESULT hr = t->GetSurfaceLevel(a ? (level >= a->skip ? level - a->skip : 0) : level, &surf);
    W32(Arg(c, 2), SUCCEEDED(hr) ? Wrap(surf) : 0);
    Return(c, uint32_t(hr), 3);
}
M(IDirect3DCubeTexture9, GetCubeMapSurface) {
    auto* t = Host<IDirect3DCubeTexture9>(Arg(c, 0));
    const TexAdapt* a = AdaptOf(t);
    UINT level = Arg(c, 2);
    IDirect3DSurface9* surf = nullptr;
    HRESULT hr = t->GetCubeMapSurface(D3DCUBEMAP_FACES(Arg(c, 1)), a ? (level >= a->skip ? level - a->skip : 0) : level,
                                      &surf);
    W32(Arg(c, 3), SUCCEEDED(hr) ? Wrap(surf) : 0);
    Return(c, uint32_t(hr), 4);
}
M(IDirect3DTexture9, LockRect) {
    // (this, Level, pLockedRect, pRect, Flags)
    auto* t = Host<IDirect3DTexture9>(Arg(c, 0));
    uint32_t level = Arg(c, 1);
    const RECT* rect = Ptr<RECT>(Arg(c, 3));
    if (const TexAdapt* a = AdaptOf(t)) {
        TexAdapt copy = *a;
        HRESULT hr = AdaptedLockRect(t, level, copy, level, rect, Arg(c, 4), Arg(c, 2),
                                     [&](UINT host_level, D3DLOCKED_RECT* lr) {
                                         return t->LockRect(host_level, lr, rect, Arg(c, 4));
                                     });
        Return(c, uint32_t(hr), 5);
        return;
    }
    D3DLOCKED_RECT lr;
    HRESULT hr = t->LockRect(level, &lr, rect, Arg(c, 4));
    D3DSURFACE_DESC desc;
    t->GetLevelDesc(level, &desc);
    LockRectCommon(c, t, level, hr, lr, desc, rect, Arg(c, 4), Arg(c, 2), 5, true);
}
M(IDirect3DTexture9, UnlockRect) {
    auto* t = Host<IDirect3DTexture9>(Arg(c, 0));
    uint32_t level = Arg(c, 1);
    const TexAdapt* a = AdaptOf(t);
    bool host_locked = true;
    if (a && AdaptedUnlock(t, level, &host_locked)) {
        Return(c, host_locked ? uint32_t(t->UnlockRect(level - a->skip)) : 0u, 2);
        return;
    }
    ShadowUnlock(t, level);
    Return(c, uint32_t(t->UnlockRect(a ? level - a->skip : level)), 2);
}
M(IDirect3DCubeTexture9, LockRect) {
    // (this, FaceType, Level, pLockedRect, pRect, Flags)
    auto* t = Host<IDirect3DCubeTexture9>(Arg(c, 0));
    uint32_t face = Arg(c, 1), level = Arg(c, 2);
    const RECT* rect = Ptr<RECT>(Arg(c, 4));
    if (const TexAdapt* a = AdaptOf(t)) {
        TexAdapt copy = *a;
        HRESULT hr = AdaptedLockRect(t, face * 64 + level, copy, level, rect, Arg(c, 5), Arg(c, 3),
                                     [&](UINT host_level, D3DLOCKED_RECT* lr) {
                                         return t->LockRect(D3DCUBEMAP_FACES(face), host_level, lr, rect, Arg(c, 5));
                                     });
        Return(c, uint32_t(hr), 6);
        return;
    }
    D3DLOCKED_RECT lr;
    HRESULT hr = t->LockRect(D3DCUBEMAP_FACES(face), level, &lr, rect, Arg(c, 5));
    D3DSURFACE_DESC desc;
    t->GetLevelDesc(level, &desc);
    LockRectCommon(c, t, face * 64 + level, hr, lr, desc, rect, Arg(c, 5), Arg(c, 3), 6, true);
}
M(IDirect3DCubeTexture9, UnlockRect) {
    auto* t = Host<IDirect3DCubeTexture9>(Arg(c, 0));
    uint32_t face = Arg(c, 1), level = Arg(c, 2);
    const TexAdapt* a = AdaptOf(t);
    bool host_locked = true;
    if (a && AdaptedUnlock(t, face * 64 + level, &host_locked)) {
        Return(c, host_locked ? uint32_t(t->UnlockRect(D3DCUBEMAP_FACES(face), level - a->skip)) : 0u, 3);
        return;
    }
    ShadowUnlock(t, face * 64 + level);
    Return(c, uint32_t(t->UnlockRect(D3DCUBEMAP_FACES(face), a ? level - a->skip : level)), 3);
}

// D3DLOCKED_BOX in guest memory: {INT RowPitch; INT SlicePitch; void* pBits} (12 bytes).
void LockBoxCommon(Ctx* c, void* object, uint32_t sub, HRESULT hr, const D3DLOCKED_BOX& lb, uint32_t depth,
                   DWORD flags, uint32_t out, int args) {
    if (SUCCEEDED(hr)) {
        W32(out, uint32_t(lb.RowPitch));
        W32(out + 4, uint32_t(lb.SlicePitch));
        W32(out + 8, ShadowLock(object, sub, lb.pBits, uint32_t(lb.SlicePitch) * depth, flags));
    }
    Return(c, uint32_t(hr), args);
}
M(IDirect3DVolumeTexture9, LockBox) {
    auto* t = Host<IDirect3DVolumeTexture9>(Arg(c, 0));
    uint32_t level = Arg(c, 1);
    const D3DBOX* box = Ptr<D3DBOX>(Arg(c, 3));
    D3DLOCKED_BOX lb;
    HRESULT hr = t->LockBox(level, &lb, box, Arg(c, 4));
    D3DVOLUME_DESC desc;
    t->GetLevelDesc(level, &desc);
    LockBoxCommon(c, t, level, hr, lb, box ? box->Back - box->Front : desc.Depth, Arg(c, 4), Arg(c, 2), 5);
}
M(IDirect3DVolumeTexture9, UnlockBox) {
    auto* t = Host<IDirect3DVolumeTexture9>(Arg(c, 0));
    ShadowUnlock(t, Arg(c, 1));
    Return(c, uint32_t(t->UnlockBox(Arg(c, 1))), 2);
}
M(IDirect3DVolume9, LockBox) {
    auto* v = Host<IDirect3DVolume9>(Arg(c, 0));
    const D3DBOX* box = Ptr<D3DBOX>(Arg(c, 2));
    D3DLOCKED_BOX lb;
    HRESULT hr = v->LockBox(&lb, box, Arg(c, 3));
    D3DVOLUME_DESC desc;
    v->GetDesc(&desc);
    LockBoxCommon(c, v, 0, hr, lb, box ? box->Back - box->Front : desc.Depth, Arg(c, 3), Arg(c, 1), 4);
}
M(IDirect3DVolume9, UnlockBox) {
    auto* v = Host<IDirect3DVolume9>(Arg(c, 0));
    ShadowUnlock(v, 0);
    Return(c, uint32_t(v->UnlockBox()), 1);
}

template <typename T, typename Desc>
void BufferLock(Ctx* c) {
    auto* b = Host<T>(Arg(c, 0));
    uint32_t offset = Arg(c, 1), size = Arg(c, 2), out = Arg(c, 3);
    DWORD flags = Arg(c, 4);
    void* p = nullptr;
    HRESULT hr = b->Lock(offset, size, &p, flags);
    if (SUCCEEDED(hr)) {
        Desc d;
        b->GetDesc(&d);
        if (offset > d.Size) offset = d.Size;
        if (!size || offset + size > d.Size) size = d.Size - offset;  // 0: to the end of the buffer
        W32(out, BufferShadowLock(b, p, d.Size, offset, size, flags, std::is_same<T, IDirect3DIndexBuffer9>::value));
    }
    Return(c, uint32_t(hr), 5);
}
template <typename T>
void BufferUnlock(Ctx* c) {
    auto* b = Host<T>(Arg(c, 0));
    ShadowUnlock(b, 0);
    Return(c, uint32_t(b->Unlock()), 1);
}
M(IDirect3DVertexBuffer9, Lock) { BufferLock<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>(c); }
M(IDirect3DVertexBuffer9, Unlock) { BufferUnlock<IDirect3DVertexBuffer9>(c); }
M(IDirect3DIndexBuffer9, Lock) { BufferLock<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>(c); }
M(IDirect3DIndexBuffer9, Unlock) { BufferUnlock<IDirect3DIndexBuffer9>(c); }

// trace_hm_draws = N: logs the first N draws into 512x512 render targets (the planet height maps) with the vertex
// shader constants c0-c3 and the positions of their vertices, to tell bad vertex data from bad GPU work.
// Read in Direct3DCreate9: settings must not be read during static initialization (the settings files are loaded
// once, on first use, and before main() the game folder is not known yet: every sporere.ini setting was ignored).
int g_trace_draws = 0;
void TraceDraw(Ctx* c, int kind) {
    static std::atomic<int> n{0};
    auto* dev = Host<IDirect3DDevice9>(Arg(c, 0));
    IDirect3DSurface9* rt = nullptr;
    D3DSURFACE_DESC rd{};
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
    rt->GetDesc(&rd);
    rt->Release();
    if (rd.Width != 512 || rd.Height != 512) return;
    if (kind == 1) {
        // the same text as the Wine-side proxy (src/host/d3d9_trace_proxy.cpp) writes for the original game
        static std::atomic<int> snaps{0};
        static FILE* f = fopen("d3d9trace.log", "w");
        int s = snaps.fetch_add(1);
        if (f && s < g_trace_draws) {
            char call[128];
            snprintf(call, sizeof call, "DrawIndexedPrimitive %d %d %u %u %u %u", int(Arg(c, 1)), int(Arg(c, 2)),
                     Arg(c, 3), Arg(c, 4), Arg(c, 5), Arg(c, 6));
            fputs((d3d9trace::Snapshot(dev, s, call) +
                   d3d9trace::Geometry(dev, int(Arg(c, 2)), Arg(c, 3), Arg(c, 4), Arg(c, 5), Arg(c, 6))).c_str(), f);
            fflush(f);
        }
    }
    int k = n.fetch_add(1);
    if (k >= g_trace_draws) return;
    IDirect3DVertexDeclaration9* decl = nullptr;
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT nel = 0;
    int pos_off = -1, pos_type = -1, pos_stream = 0;
    std::string elems;
    if (SUCCEEDED(dev->GetVertexDeclaration(&decl)) && decl) {
        decl->GetDeclaration(el, &nel);
        decl->Release();
        for (UINT i = 0; i < nel && el[i].Stream != 0xff; ++i) {
            char e[48];
            snprintf(e, sizeof e, " s%u+%u:t%u:u%u.%u", el[i].Stream, el[i].Offset, el[i].Type, el[i].Usage,
                     el[i].UsageIndex);
            elems += e;
            if ((el[i].Usage == D3DDECLUSAGE_POSITION || el[i].Usage == D3DDECLUSAGE_POSITIONT) && pos_off < 0) {
                pos_off = el[i].Offset;
                pos_type = el[i].Type;
                pos_stream = el[i].Stream;
            }
        }
    }
    float vc[16] = {};
    dev->GetVertexShaderConstantF(0, vc, 4);
    uint32_t prim = Arg(c, 1);
    Log("hm draw %d kind %d prim %u args %u %u %u %u %u | decl%s | c0 %g %g %g %g c1 %g %g %g %g c2 %g %g %g %g c3 %g %g "
        "%g %g", k, kind, prim, Arg(c, 2), Arg(c, 3), Arg(c, 4), Arg(c, 5), Arg(c, 6), elems.c_str(), vc[0], vc[1],
        vc[2], vc[3], vc[4], vc[5], vc[6], vc[7], vc[8], vc[9], vc[10], vc[11], vc[12], vc[13], vc[14], vc[15]);
    if (k < 40) {
        std::string st;
        for (DWORD s = 0; s < 4; ++s) {
            IDirect3DBaseTexture9* t = nullptr;
            if (FAILED(dev->GetTexture(s, &t)) || !t) continue;
            D3DFORMAT f = D3DFMT_UNKNOWN;
            UINT w = 0;
            if (t->GetType() == D3DRTYPE_TEXTURE) {
                D3DSURFACE_DESC d{};
                static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d);
                f = d.Format;
                w = d.Width;
            }
            t->Release();
            DWORD au = 0, av = 0, bc = 0, mn = 0, mg = 0, mp = 0, srgb = 0;
            dev->GetSamplerState(s, D3DSAMP_ADDRESSU, &au);
            dev->GetSamplerState(s, D3DSAMP_ADDRESSV, &av);
            dev->GetSamplerState(s, D3DSAMP_BORDERCOLOR, &bc);
            dev->GetSamplerState(s, D3DSAMP_MINFILTER, &mn);
            dev->GetSamplerState(s, D3DSAMP_MAGFILTER, &mg);
            dev->GetSamplerState(s, D3DSAMP_MIPFILTER, &mp);
            dev->GetSamplerState(s, D3DSAMP_SRGBTEXTURE, &srgb);
            char e[160];
            snprintf(e, sizeof e, " t%lu:fmt%u/%u addr%lu,%lu border%08lx filt%lu/%lu/%lu srgb%lu", s, unsigned(f), w, au, av,
                     bc, mn, mg, mp, srgb);
            st += e;
        }
        DWORD be = 0, sb = 0, db = 0, op = 0, sbe = 0, at = 0, cw = 0;
        dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &be);
        dev->GetRenderState(D3DRS_SRCBLEND, &sb);
        dev->GetRenderState(D3DRS_DESTBLEND, &db);
        dev->GetRenderState(D3DRS_BLENDOP, &op);
        dev->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &sbe);
        dev->GetRenderState(D3DRS_ALPHATESTENABLE, &at);
        dev->GetRenderState(D3DRS_COLORWRITEENABLE, &cw);
        Log("hm state %d:%s | blend %lu %lu/%lu op%lu sep%lu atest%lu cw%lx", k, st.c_str(), be, sb, db, op, sbe, at, cw);
    }
    if (pos_off < 0 || (pos_type != D3DDECLTYPE_FLOAT2 && pos_type != D3DDECLTYPE_FLOAT3 && pos_type != D3DDECLTYPE_FLOAT4))
        return;
    int comps = pos_type == D3DDECLTYPE_FLOAT2 ? 2 : pos_type == D3DDECLTYPE_FLOAT3 ? 3 : 4;
    uint32_t base = 0, stride = 0, first = 0, count = 0;
    if (kind == 2) {  // DrawPrimitiveUP(type, primCount, data, stride)
        base = Arg(c, 3);
        stride = Arg(c, 4);
        count = 6;
    } else if (kind == 3) {  // DrawIndexedPrimitiveUP(type, minIndex, numVertices, primCount, indices, fmt, data, stride)
        base = Arg(c, 7);
        stride = Arg(c, 8);
        first = Arg(c, 2);
        count = std::min<uint32_t>(Arg(c, 3), 8);
    } else {
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT off = 0, st = 0;
        if (FAILED(dev->GetStreamSource(pos_stream, &vb, &off, &st)) || !vb) return;
        {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            auto it = g_shadows.find({vb, 0});
            if (it != g_shadows.end()) base = it->second.guest + off;
        }
        vb->Release();
        stride = st;
        first = kind == 0 ? Arg(c, 2) : Arg(c, 2) + Arg(c, 3);  // DrawPrimitive start / BaseVertexIndex + MinIndex
        count = std::min<uint32_t>(kind == 0 ? 6 : Arg(c, 4), 8);
    }
    if (kind == 1) {
        // DrawIndexedPrimitive(type, BaseVertexIndex, MinIndex, NumVertices, startIndex, primCount): indices from the
        // index buffer's mirror, checked against the range the draw declares
        IDirect3DIndexBuffer9* ib = nullptr;
        if (SUCCEEDED(dev->GetIndices(&ib)) && ib) {
            D3DINDEXBUFFER_DESC id{};
            ib->GetDesc(&id);
            uint32_t ibase = 0;
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                auto it = g_shadows.find({ib, 0});
                if (it != g_shadows.end()) ibase = it->second.guest;
            }
            ib->Release();
            uint32_t isz = id.Format == D3DFMT_INDEX32 ? 4 : 2, n = Arg(c, 6) * 3, lo = Arg(c, 3), hi = lo + Arg(c, 4);
            std::string ix;
            int out_of_range = 0;
            for (uint32_t i = 0; ibase && i < n; ++i) {
                uint32_t a = ibase + (Arg(c, 5) + i) * isz, v = isz == 4 ? R32(a) : R16(a);
                if (v < lo || v >= hi) ++out_of_range;
                if (i < 24) ix += " " + std::to_string(v);
            }
            Log("hm idx %d%s%s:%s", k, ibase ? "" : " (no mirror)", out_of_range ? " OUT OF RANGE" : "", ix.c_str());
        }
    }
    if (!base || !stride) return;
    std::string v;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t a = base + (first + i) * stride;
        v += " [";
        for (uint32_t w = 0; w + 4 <= stride && w < 64; w += 4) {
            char t[24];
            snprintf(t, sizeof t, w ? " %.4g" : "%.4g", RF32(a + w));
            v += t;
        }
        v += "]";
        (void)comps;
    }
    Log("hm verts %d stride %u:%s", k, stride, v.c_str());
}

#undef M

#include "d3d9_gen.inc"

namespace {

// Builds the guest vtables (one thunk per method) and registers Direct3DCreate9.
struct Registrar {
    Registrar() {
        static const ImportDef create[] = {{"d3d9.dll", "Direct3DCreate9", x_Direct3DCreate9}};
        RegisterImports(create, 1);
        for (const InterfaceDef& i : kInterfaces) {
            std::vector<ImportDef> defs;
            for (size_t k = 0; k < i.count; ++k) {
                std::string n = std::string(i.name) + "::" + i.methods[k].name;
                defs.push_back({"d3d9.com", strdup(n.c_str()), i.methods[k].fn});
            }
            RegisterImports(defs.data(), defs.size());
        }
    }
} g_registrar;

}  // namespace

// Called once guest memory exists (the vtables live there).
void InitD3D9Bridge() {
    for (const InterfaceDef& i : kInterfaces) {
        uint32_t vt = HeapAlloc(uint32_t(4 * i.count));
        for (size_t k = 0; k < i.count; ++k) {
            std::string n = std::string(i.name) + "::" + i.methods[k].name;
            W32(vt + uint32_t(4 * k), ImportThunk("d3d9.com", n.c_str()));
        }
        g_vtables[i.name] = vt;
    }
}

}  // namespace native
