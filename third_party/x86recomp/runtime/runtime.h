/* Runtime for statically recompiled engine code (tools/recomp/recomp.py output).
 *
 * Every recompiled function has the signature `void f_<va>(Ctx*)` and works on an explicit guest CPU state.
 * Guest memory is addressed with 32-bit guest addresses relative to RecompMemoryBase: 0 when running inside the
 * original 32-bit process (prototype, hooked into the engine). The native host (src/native) builds with
 * RECOMP_FLAT_MEMORY and keeps guest memory in a 4 GiB window of its own address space at the compile-time constant
 * RECOMP_GUEST_BASE. */
#ifndef X86RECOMP_RUNTIME_H
#define X86RECOMP_RUNTIME_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union XmmValue {
    float f[4];
    double d[2];
    uint32_t u32[4];
    uint64_t u64[2];
} XmmValue;

typedef struct Ctx {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint8_t cf, zf, sf, of, pf, df, af;
    uint8_t pad;
    uint32_t ret_target;  /* guest return address popped by the last `ret`, used by the native bridge */
    uint32_t fs_base;     /* guest address of the TEB (segment fs) */
    /* x87: registers in double precision, hardware-style TOP, status (C0..C3) and control words */
    double st[8];
    uint32_t st_top;
    uint16_t fpu_sw, fpu_cw;
    XmmValue xmm[8];
    /* MMX registers, kept apart from the x87 stack (the engines never move data between the two through the
     * aliased register file; emms is a no-op) */
    uint64_t mm[8];
    uint32_t mxcsr;
    /* the running guest thread's stack [stack_lo, stack_hi), set by the host (B* accessors below) */
    uint32_t stack_lo, stack_hi;
} Ctx;

extern uint8_t* RecompMemoryBase;

typedef void (*RecompFn)(Ctx*);

/* Calls guest address `target`: a recompiled function when one exists, otherwise the host fallback (the
 * original native code in the prototype). The caller has already pushed the return address. */
void RecompDispatch(Ctx* ctx, uint32_t target);

/* Looks up the recompiled implementation of a guest function entry, or null. */
RecompFn RecompLookup(uint32_t target);

/* Direct calls between recompiled functions are allowed unless the embedding restricts them (bisect range, test
 * detours in the in-process prototype). */
extern volatile uint32_t RecompRestrictLo, RecompRestrictHi, RecompTestDetours;
#ifdef RECOMP_FLAT_MEMORY
/* Native host: every recompiled function is always called directly (the restriction exists for Wine bisects). */
static inline int RecompDirect(uint32_t target) { (void)target; return 1; }
#else
static inline int RecompDirect(uint32_t target) {
    return RecompTestDetours == 0 && target >= RecompRestrictLo && target < RecompRestrictHi;
}
#endif

/* ---- memory ---- */
#ifdef RECOMP_FLAT_MEMORY
/* Native host: guest memory is a 4 GiB window at RECOMP_GUEST_BASE (a constant, so the addition folds into the
 * addressing); 0 puts it in the low 4 GiB, where Android's Java heap and Qualcomm's GPU driver also live. */
#ifndef RECOMP_GUEST_BASE
#define RECOMP_GUEST_BASE 0
#endif
static inline uint8_t* G(uint32_t a) { return (uint8_t*)(uintptr_t)((uint64_t)RECOMP_GUEST_BASE + a); }
#else
static inline uint8_t* G(uint32_t a) { return RecompMemoryBase + a; }
#endif
#if defined(RECOMP_TSO)
/* Weakly ordered hosts (arm64): x86 code relies on total store order (a store is never seen before an earlier one,
 * a load never runs ahead of an earlier load), e.g. a worker that sees a "ready" flag before the data it guards.
 * Guest loads are acquire and stores release, which gives the same guarantees; unaligned accesses (rare) use
 * plain copies fenced on the right side. */
#define RECOMP_ALIGNED(a, n) ((((uintptr_t)G(a)) & ((n) - 1)) == 0)
static inline uint8_t R8(uint32_t a) { return __atomic_load_n(G(a), __ATOMIC_ACQUIRE); }
static inline uint16_t R16(uint32_t a) {
    if (RECOMP_ALIGNED(a, 2)) return __atomic_load_n((uint16_t*)G(a), __ATOMIC_ACQUIRE);
    uint16_t v; memcpy(&v, G(a), 2); __atomic_thread_fence(__ATOMIC_ACQUIRE); return v;
}
static inline uint32_t R32(uint32_t a) {
    if (RECOMP_ALIGNED(a, 4)) return __atomic_load_n((uint32_t*)G(a), __ATOMIC_ACQUIRE);
    uint32_t v; memcpy(&v, G(a), 4); __atomic_thread_fence(__ATOMIC_ACQUIRE); return v;
}
static inline uint64_t R64(uint32_t a) {
    if (RECOMP_ALIGNED(a, 8)) return __atomic_load_n((uint64_t*)G(a), __ATOMIC_ACQUIRE);
    uint64_t v; memcpy(&v, G(a), 8); __atomic_thread_fence(__ATOMIC_ACQUIRE); return v;
}
static inline float RF32(uint32_t a) { uint32_t b = R32(a); float v; memcpy(&v, &b, 4); return v; }
static inline double RF64(uint32_t a) { uint64_t b = R64(a); double v; memcpy(&v, &b, 8); return v; }
static inline void W8(uint32_t a, uint8_t v) { __atomic_store_n(G(a), v, __ATOMIC_RELEASE); }
static inline void W16(uint32_t a, uint16_t v) {
    if (RECOMP_ALIGNED(a, 2)) { __atomic_store_n((uint16_t*)G(a), v, __ATOMIC_RELEASE); return; }
    __atomic_thread_fence(__ATOMIC_RELEASE); memcpy(G(a), &v, 2);
}
static inline void W32(uint32_t a, uint32_t v) {
    if (RECOMP_ALIGNED(a, 4)) { __atomic_store_n((uint32_t*)G(a), v, __ATOMIC_RELEASE); return; }
    __atomic_thread_fence(__ATOMIC_RELEASE); memcpy(G(a), &v, 4);
}
static inline void W64(uint32_t a, uint64_t v) {
    if (RECOMP_ALIGNED(a, 8)) { __atomic_store_n((uint64_t*)G(a), v, __ATOMIC_RELEASE); return; }
    __atomic_thread_fence(__ATOMIC_RELEASE); memcpy(G(a), &v, 8);
}
static inline void WF32(uint32_t a, float v) { uint32_t b; memcpy(&b, &v, 4); W32(a, b); }
static inline void WF64(uint32_t a, double v) { uint64_t b; memcpy(&b, &v, 8); W64(a, b); }
/* The guest stack is private to its thread: pushes, pops and esp-relative accesses (locals, arguments) stay plain. */
static inline void StackW32(uint32_t a, uint32_t v) { memcpy(G(a), &v, 4); }
static inline uint32_t StackR32(uint32_t a) { uint32_t v; memcpy(&v, G(a), 4); return v; }
static inline uint8_t SR8(uint32_t a) { return *G(a); }
static inline uint16_t SR16(uint32_t a) { uint16_t v; memcpy(&v, G(a), 2); return v; }
static inline uint32_t SR32(uint32_t a) { uint32_t v; memcpy(&v, G(a), 4); return v; }
static inline uint64_t SR64(uint32_t a) { uint64_t v; memcpy(&v, G(a), 8); return v; }
static inline float SRF32(uint32_t a) { float v; memcpy(&v, G(a), 4); return v; }
static inline double SRF64(uint32_t a) { double v; memcpy(&v, G(a), 8); return v; }
static inline void SW8(uint32_t a, uint8_t v) { *G(a) = v; }
static inline void SW16(uint32_t a, uint16_t v) { memcpy(G(a), &v, 2); }
static inline void SW32(uint32_t a, uint32_t v) { memcpy(G(a), &v, 4); }
static inline void SW64(uint32_t a, uint64_t v) { memcpy(G(a), &v, 8); }
static inline void SWF32(uint32_t a, float v) { memcpy(G(a), &v, 4); }
static inline void SWF64(uint32_t a, double v) { memcpy(G(a), &v, 8); }
#else
static inline uint8_t R8(uint32_t a) { return *G(a); }
static inline uint16_t R16(uint32_t a) { uint16_t v; memcpy(&v, G(a), 2); return v; }
static inline uint32_t R32(uint32_t a) { uint32_t v; memcpy(&v, G(a), 4); return v; }
static inline uint64_t R64(uint32_t a) { uint64_t v; memcpy(&v, G(a), 8); return v; }
static inline float RF32(uint32_t a) { float v; memcpy(&v, G(a), 4); return v; }
static inline double RF64(uint32_t a) { double v; memcpy(&v, G(a), 8); return v; }
static inline void W8(uint32_t a, uint8_t v) { *G(a) = v; }
static inline void W16(uint32_t a, uint16_t v) { memcpy(G(a), &v, 2); }
static inline void W32(uint32_t a, uint32_t v) { memcpy(G(a), &v, 4); }
static inline void W64(uint32_t a, uint64_t v) { memcpy(G(a), &v, 8); }
static inline void WF32(uint32_t a, float v) { memcpy(G(a), &v, 4); }
static inline void WF64(uint32_t a, double v) { memcpy(G(a), &v, 8); }

#define StackW32 W32
#define StackR32 R32
#define SR8 R8
#define SR16 R16
#define SR32 R32
#define SR64 R64
#define SRF32 RF32
#define SRF64 RF64
#define SW8 W8
#define SW16 W16
#define SW32 W32
#define SW64 W64
#define SWF32 WF32
#define SWF64 WF64
#endif

static inline void Push32(Ctx* c, uint32_t v) { c->esp -= 4; StackW32(c->esp, v); }
static inline uint32_t Pop32(Ctx* c) { uint32_t v = StackR32(c->esp); c->esp += 4; return v; }

/* Register localization (tools/recomp/recomp.py): inside a recompiled function the eight general registers and the
 * arithmetic flags live in C locals, which the host compiler keeps in machine registers (guest memory writes cannot alias them, and TSO
 * barriers do not apply to them). They are written back to the context before anything that reads it - calls out
 * of the function, the dispatcher, traps, returns - and reloaded after calls. */
#define RLOCALS uint32_t r_eax = c->eax, r_ecx = c->ecx, r_edx = c->edx, r_ebx = c->ebx, r_esp = c->esp, \
                         r_ebp = c->ebp, r_esi = c->esi, r_edi = c->edi; \
                RFlags rf = {c->cf, c->zf, c->sf, c->of, c->pf, c->df, c->af}
#define RSPILL (c->eax = r_eax, c->ecx = r_ecx, c->edx = r_edx, c->ebx = r_ebx, c->esp = r_esp, c->ebp = r_ebp, \
                c->esi = r_esi, c->edi = r_edi, c->cf = rf.cf, c->zf = rf.zf, c->sf = rf.sf, c->of = rf.of, \
                c->pf = rf.pf, c->df = rf.df, c->af = rf.af)
#define RRELOAD (r_eax = c->eax, r_ecx = c->ecx, r_edx = c->edx, r_ebx = c->ebx, r_esp = c->esp, r_ebp = c->ebp, \
                 r_esi = c->esi, r_edi = c->edi, rf.cf = c->cf, rf.zf = c->zf, rf.sf = c->sf, rf.of = c->of, \
                 rf.pf = c->pf, rf.df = c->df, rf.af = c->af)
/* Before a jump into another function: the local flags back to the context (that function's RLOCALS reads them). */
#define RFLAGS_OUT (c->cf = rf.cf, c->zf = rf.zf, c->sf = rf.sf, c->of = rf.of, c->pf = rf.pf, c->df = rf.df, \
                    c->af = rf.af)
static inline void LPush32(uint32_t* esp, uint32_t v) { *esp -= 4; StackW32(*esp, v); }
static inline uint32_t LPop32(uint32_t* esp) { uint32_t v = StackR32(*esp); *esp += 4; return v; }

/* ---- flags ---- */
static inline uint8_t Parity(uint32_t v) {
    v &= 0xff;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (uint8_t)(~v & 1);
}

#define RECOMP_FLAGS_LOGIC(c, r, bits)                                   \
    do {                                                                 \
        (c)->cf = 0;                                                     \
        (c)->of = 0;                                                     \
        (c)->zf = ((r) & (uint32_t)((1ull << (bits)) - 1)) == 0;         \
        (c)->sf = (((r) >> ((bits) - 1)) & 1);                           \
        (c)->pf = Parity(r);                                             \
    } while (0)

static inline uint32_t Mask(int bits) { return bits == 32 ? 0xffffffffu : ((1u << bits) - 1); }

/* ---- 128-bit memory ---- */
static inline XmmValue RecompLoad128(uint32_t a) { XmmValue v; memcpy(&v, G(a), 16); return v; }
static inline void RecompStore128(uint32_t a, XmmValue v) { memcpy(G(a), &v, 16); }

/* Frame-pointer accesses ([ebp + k], recomp.py): usually the function's own locals, which no other thread sees, so
 * they need none of the ordering the TSO accessors pay for. ebp can also be a general register in optimized code,
 * so the address is checked against the thread's stack first; anything else keeps the ordered access. */
#define RECOMP_ON_STACK(c, a) ((uint32_t)((a) - (c)->stack_lo) < (uint32_t)((c)->stack_hi - (c)->stack_lo))
static inline uint8_t BR8(const Ctx* c, uint32_t a) { return RECOMP_ON_STACK(c, a) ? SR8(a) : R8(a); }
static inline uint16_t BR16(const Ctx* c, uint32_t a) { return RECOMP_ON_STACK(c, a) ? SR16(a) : R16(a); }
static inline uint32_t BR32(const Ctx* c, uint32_t a) { return RECOMP_ON_STACK(c, a) ? SR32(a) : R32(a); }
static inline uint64_t BR64(const Ctx* c, uint32_t a) { return RECOMP_ON_STACK(c, a) ? SR64(a) : R64(a); }
static inline float BRF32(const Ctx* c, uint32_t a) { return RECOMP_ON_STACK(c, a) ? SRF32(a) : RF32(a); }
static inline double BRF64(const Ctx* c, uint32_t a) { return RECOMP_ON_STACK(c, a) ? SRF64(a) : RF64(a); }
static inline void BW8(const Ctx* c, uint32_t a, uint8_t v) { if (RECOMP_ON_STACK(c, a)) SW8(a, v); else W8(a, v); }
static inline void BW16(const Ctx* c, uint32_t a, uint16_t v) { if (RECOMP_ON_STACK(c, a)) SW16(a, v); else W16(a, v); }
static inline void BW32(const Ctx* c, uint32_t a, uint32_t v) { if (RECOMP_ON_STACK(c, a)) SW32(a, v); else W32(a, v); }
static inline void BW64(const Ctx* c, uint32_t a, uint64_t v) { if (RECOMP_ON_STACK(c, a)) SW64(a, v); else W64(a, v); }
static inline void BWF32(const Ctx* c, uint32_t a, float v) { if (RECOMP_ON_STACK(c, a)) SWF32(a, v); else WF32(a, v); }
static inline void BWF64(const Ctx* c, uint32_t a, double v) { if (RECOMP_ON_STACK(c, a)) SWF64(a, v); else WF64(a, v); }

/* Natural alignment of a guest address on the host (for every build, TSO or not). */
#define RECOMP_IS_ALIGNED(a, n) ((((uintptr_t)G(a)) & ((n) - 1)) == 0)

/* Locked operations at any alignment. x86 allows them on misaligned addresses; ARM's atomic instructions fault there
 * (SIGBUS). Aligned ones use the host atomics; misaligned ones take a spinlock from one table keyed by the address
 * (defined once by the host, RecompMisalignedLocks), so they stay atomic against each other. */
extern uint8_t RecompMisalignedLocks[256];
static inline void RecompLockAddr(uint32_t a) {
    uint8_t* l = &RecompMisalignedLocks[(a >> 3) & 255u];
    while (__atomic_test_and_set(l, __ATOMIC_ACQUIRE)) {
    }
}
static inline void RecompUnlockAddr(uint32_t a) { __atomic_clear(&RecompMisalignedLocks[(a >> 3) & 255u], __ATOMIC_RELEASE); }
enum { RECOMP_RMW_ADD, RECOMP_RMW_SUB, RECOMP_RMW_OR, RECOMP_RMW_AND, RECOMP_RMW_XOR, RECOMP_RMW_XCHG };
/* Returns the old value. */
static inline uint32_t RecompRmw32(uint32_t a, int kind, uint32_t b) {
    uint32_t* p = (uint32_t*)G(a);
    if (RECOMP_IS_ALIGNED(a, 4)) {
        switch (kind) {
            case RECOMP_RMW_ADD: return __atomic_fetch_add(p, b, __ATOMIC_SEQ_CST);
            case RECOMP_RMW_SUB: return __atomic_fetch_sub(p, b, __ATOMIC_SEQ_CST);
            case RECOMP_RMW_OR: return __atomic_fetch_or(p, b, __ATOMIC_SEQ_CST);
            case RECOMP_RMW_AND: return __atomic_fetch_and(p, b, __ATOMIC_SEQ_CST);
            case RECOMP_RMW_XOR: return __atomic_fetch_xor(p, b, __ATOMIC_SEQ_CST);
            default: return __atomic_exchange_n(p, b, __ATOMIC_SEQ_CST);
        }
    }
    RecompLockAddr(a);
    uint32_t old, v;
    memcpy(&old, p, 4);
    switch (kind) {
        case RECOMP_RMW_ADD: v = old + b; break;
        case RECOMP_RMW_SUB: v = old - b; break;
        case RECOMP_RMW_OR: v = old | b; break;
        case RECOMP_RMW_AND: v = old & b; break;
        case RECOMP_RMW_XOR: v = old ^ b; break;
        default: v = b; break;
    }
    memcpy(p, &v, 4);
    RecompUnlockAddr(a);
    return old;
}
static inline uint32_t RecompCas32(uint32_t a, uint32_t expected, uint32_t desired) {
    uint32_t* p = (uint32_t*)G(a);
    if (RECOMP_IS_ALIGNED(a, 4)) return __sync_val_compare_and_swap(p, expected, desired);
    RecompLockAddr(a);
    uint32_t old;
    memcpy(&old, p, 4);
    if (old == expected) memcpy(p, &desired, 4);
    RecompUnlockAddr(a);
    return old;
}
static inline uint64_t RecompCas64(uint32_t a, uint64_t expected, uint64_t desired) {
    uint64_t* p = (uint64_t*)G(a);
    if (RECOMP_IS_ALIGNED(a, 8)) return __sync_val_compare_and_swap(p, expected, desired);
    RecompLockAddr(a);
    uint64_t old;
    memcpy(&old, p, 8);
    if (old == expected) memcpy(p, &desired, 8);
    RecompUnlockAddr(a);
    return old;
}

/* Bulk memory (inlined CRT memcpy/memmove/memset, rep movs/stos): plain host memmove/memset between fences, so the
 * block as a whole keeps x86's ordering against the accesses around it (the bytes inside need no order of their own:
 * x86 makes no promise about them either). */
static inline uint32_t RecompMemmove(uint32_t d, uint32_t s, uint32_t n) {
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
    memmove(G(d), G(s), n);
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
    return d;
}
static inline uint32_t RecompMemset(uint32_t d, uint32_t v, uint32_t n) {
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
    memset(G(d), (int)(v & 0xffu), n);
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
    return d;
}
/* rep movs / rep stos with DF clear, the whole instruction: one block between fences when source and destination do
 * not overlap (or for a fill), the element loop otherwise (an overlapping forward copy repeats a pattern, which a
 * memmove would not). They return nothing: the recompiler wraps calls that see the context in register spills. */
static inline void RecompRepMovs(Ctx* c, uint32_t width) {
    uint32_t n = c->ecx * width, d = c->edi, s = c->esi;
    if (!c->ecx) return;
    if ((d + n <= s || s + n <= d) && n / width == c->ecx) {
        RecompMemmove(d, s, n);
        c->esi = s + n;
        c->edi = d + n;
        c->ecx = 0;
        return;
    }
    while (c->ecx) {
        if (width == 4) W32(c->edi, R32(c->esi));
        else if (width == 2) W16(c->edi, R16(c->esi));
        else W8(c->edi, R8(c->esi));
        c->esi += width;
        c->edi += width;
        c->ecx--;
    }
}
static inline void RecompRepStos(Ctx* c, uint32_t width) {
    uint32_t count = c->ecx, d = c->edi;
    if (!count) return;
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
    if (width == 1) {
        memset(G(d), (int)(c->eax & 0xffu), count);
    } else {
        uint32_t v = width == 4 ? c->eax : (c->eax & 0xffffu) * 0x00010001u;
        uint8_t* p = G(d);
        for (uint32_t i = 0; i < count; ++i) memcpy(p + (size_t)i * width, &v, width);
    }
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
    c->edi = d + count * width;
    c->ecx = 0;
}

/* Flag helpers exist twice: on the context, and on a function's local copy of the flags (RFlags, see RLOCALS),
 * which the host compiler keeps in registers. Same bodies; only the flag fields are used. */
typedef struct RFlags {
    uint8_t cf, zf, sf, of, pf, df, af;
} RFlags;
#define RECOMP_FLAG_FUNCS(S, T) \
    static inline uint32_t AddFlags##S(T* c, uint32_t a, uint32_t b, uint32_t carry, int bits) { \
        uint64_t wide = (uint64_t)(a & Mask(bits)) + (b & Mask(bits)) + carry; \
        uint32_t r = (uint32_t)wide & Mask(bits); \
        uint32_t sign = 1u << (bits - 1); \
        c->cf = (wide >> bits) & 1; \
        c->zf = r == 0; \
        c->sf = (r & sign) != 0; \
        c->of = ((~(a ^ b) & (a ^ r)) & sign) != 0; \
        c->pf = Parity(r); \
        return r; \
    } \
     \
    static inline uint32_t SubFlags##S(T* c, uint32_t a, uint32_t b, uint32_t borrow, int bits) { \
        uint32_t am = a & Mask(bits), bm = b & Mask(bits); \
        uint64_t wide = (uint64_t)am - bm - borrow; \
        uint32_t r = (uint32_t)wide & Mask(bits); \
        uint32_t sign = 1u << (bits - 1); \
        c->cf = (uint64_t)am < (uint64_t)bm + borrow; \
        c->zf = r == 0; \
        c->sf = (r & sign) != 0; \
        c->of = (((a ^ b) & (a ^ r)) & sign) != 0; \
        c->pf = Parity(r); \
        return r; \
    } \
     \
     \
    static inline uint8_t RecompLahf##S(const T* c) { \
        return (uint8_t)((c->sf << 7) | (c->zf << 6) | (c->af << 4) | (c->pf << 2) | 2u | c->cf); \
    } \
    static inline void RecompSahf##S(T* c, uint8_t ah) { \
        c->sf = (ah >> 7) & 1; c->zf = (ah >> 6) & 1; c->af = (ah >> 4) & 1; c->pf = (ah >> 2) & 1; c->cf = ah & 1; \
    } \
    static inline uint32_t RecompEflags##S(const T* c) { \
        return RecompLahf##S(c) | ((uint32_t)c->df << 10) | ((uint32_t)c->of << 11) | 0x200u; \
    } \
    static inline void RecompSetEflags##S(T* c, uint32_t f) { \
        RecompSahf##S(c, (uint8_t)f); \
        c->df = (f >> 10) & 1; \
        c->of = (f >> 11) & 1; \
    }
RECOMP_FLAG_FUNCS(, Ctx)
RECOMP_FLAG_FUNCS(L, RFlags)



/* ---- setjmp/longjmp (MSVCR80 _setjmp3/longjmp, used by the engine's Lua) ----
 * The guest jmp_buf is never written: the generated code calls the host setjmp in the recompiled function's own
 * frame on a slot keyed by the guest buffer address, and longjmp unwinds the host C frames back to it. */
#include <setjmp.h>

typedef struct RecompJmpSlot {
    uint32_t guest_buf;
    uint32_t esp, ebp, ebx, esi, edi, st_top;
    uint16_t fpu_cw;
    uint32_t value;
    uint8_t bridge_state[32];  /* embedding-specific (host stack bookkeeping), restored on longjmp */
    jmp_buf host;
} RecompJmpSlot;

/* Saves the guest callee-saved registers for `guest_buf` and returns the host jmp_buf to setjmp on. */
jmp_buf* RecompSetjmpPrepare(Ctx* ctx, uint32_t guest_buf);
/* Restores the guest registers after a longjmp landed on `guest_buf`; eax receives the value. */
void RecompSetjmpResume(Ctx* ctx, uint32_t guest_buf);
/* longjmp(guest_buf, value): does not return. */
void RecompLongjmp(Ctx* ctx, uint32_t guest_buf, uint32_t value) __attribute__((noreturn));

/* ---- host services provided by the embedding ---- */
void RecompUnimplemented(Ctx* ctx, uint32_t va, const char* what);
void RecompCpuid(Ctx* ctx);
void RecompBridgeSave(uint8_t state[32]);
void RecompBridgeRestore(const uint8_t state[32]);

#ifdef __cplusplus
}
#endif

#include "runtime_simd.h"

#endif
