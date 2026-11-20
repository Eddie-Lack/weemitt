/* Pointers in the game's memory (docs/port64.md, "Ptr32"). port_ram holds the N64's RDRAM and the game's heap at the
   N64 offsets, and its structures keep the N64's layout, with 4-byte pointers. A pointer stored there is a 32-bit
   word W:

       W = 0 for NULL, else PORT_PTR_BASE + (p - port_ram), which is p - PORT_PTR_DELTA.

   On the normal 32-bit builds PORT_PTR_BASE is the address of port_ram and the delta is 0: W is the pointer's own
   bits, as before. The displaced test build (make headless-displaced) puts port_ram 32 MB above PORT_PTR_BASE, and a
   64-bit build or the library (PORT_LIB, also 32-bit) puts it anywhere; W is then a different number from the
   pointer.

   In C++ the types Ptr32<T>, Fn32<F> and PortMesg hold such a word and convert to and from T *. The game's headers
   write PTR32(T), FN32(F) and MESG32 for the members and globals that live in port_ram. In C, on the normal builds,
   these are plain pointers. In C on an encoded build they are bare words, and '->' through one does not compile.

   Rules: a Ptr32, Fn32 or PortMesg is never a function's parameter or return type (on i386 a structure is returned
   through memory, which changes the calling convention). Locals stay T *. */
#ifndef PORT_PTR32_H
#define PORT_PTR32_H

#if defined(__LP64__) || defined(_WIN64) || defined(PORT_PTR_DISPLACED) || defined(PORT_LIB)
#define PORT_PTR_ENCODED 1
#endif

#if defined(__cplusplus) && !defined(PORT_PTR32_ALIAS) && !defined(PORT_TYPES_ONLY)
#define PTR32(T) Ptr32<T>
#define FN32(F) Fn32<F>
#define MESG32 PortMesg
#elif defined(PORT_TYPES_ONLY) || !defined(PORT_PTR_ENCODED) || defined(__cplusplus)
#define PTR32(T) T *
#define FN32(F) F *
#define MESG32 OSMesg
#else
#define PTR32(T) u32
#define FN32(F) u32
#define MESG32 u32
#endif

/* A function in a table of functions (port_fn_table, port_audio_fn_table). */
typedef void (*PortFn)(void);

#ifndef PORT_TYPES_ONLY
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char port_ram[];
/* The end of the window that a word can point into: port_ram, port_hw and port_win (gen/ram.S). */
extern unsigned char port_win_end[];
void port_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* The port's functions that the game's data points to, in order of first use (gen/ramtab.c). */
extern const PortFn port_fn_table[];
extern const unsigned int port_fn_count;
/* The functions that the sound player's structures point to: its filters, its DMA routine and its player handler
   (mus-glue.c; audio_stub.c has none). Their indices follow port_fn_table's. */
extern const PortFn port_audio_fn_table[];
extern const unsigned int port_audio_fn_count;
#ifdef PORT_PTR_ENCODED
/* A function's word is PORT_FN_BASE + 4 * its index in port_fn_table, or port_fn_count + its index in
   port_audio_fn_table (loaders.c). */
unsigned int port_fn_word(PortFn fn);
#endif
#ifdef __cplusplus
}
#endif

/* The word of a pointer and the pointer of a word. */
#if defined(PORT_PTR_DISPLACED)
#define PORT_PTR_BASE 0x0C000000u
#define PORT_PTR_DELTA ((uintptr_t)0x02000000u)
#elif defined(PORT_PTR_ENCODED)
#define PORT_PTR_BASE 0x0C000000u
extern uintptr_t port_ptr_delta;
#define PORT_PTR_DELTA port_ptr_delta
#else
#define PORT_PTR_BASE ((unsigned int)(uintptr_t)port_ram)
#define PORT_PTR_DELTA ((uintptr_t)0)
#endif
#define PORT_FN_BASE 0x0BF00000u

#ifdef PORT_PTR_ENCODED
/* With PORT_PTR_CHECK, a pointer outside the window is fatal here too. PORT_P2W is also used without a Ptr32. */
static inline __attribute__((always_inline)) unsigned int port_p2w(const volatile void *p) {
#ifdef PORT_PTR_CHECK
    if (p && ((const unsigned char *)p < port_ram || (const unsigned char *)p >= port_win_end)) {
        port_fatal("PORT_P2W %p from %p", (const void *)p, __builtin_return_address(0));
    }
#endif
    return p ? (unsigned int)((uintptr_t)p - PORT_PTR_DELTA) : 0u;
}
static inline void *port_w2p(unsigned int w) {
    return w ? (void *)((uintptr_t)w + PORT_PTR_DELTA) : (void *)0;
}
#define PORT_P2W(p) port_p2w(p)
#define PORT_W2P(T, w) ((T *)port_w2p((unsigned int)(w)))
#define PORT_FN_WORD(i) (PORT_FN_BASE + 4u * (i))
#else
#define PORT_P2W(p) ((unsigned int)(uintptr_t)(p))
#define PORT_W2P(T, w) ((T *)(uintptr_t)(unsigned int)(w))
#define PORT_FN_WORD(i) ((unsigned int)(uintptr_t)port_fn_table[i])
#endif

/* A ROM or PI address that the game keeps in a pointer (the xxxRomStart symbols, port_romsyms.h) as the number. It is
   not a host address and is never decoded. */
#define PORT_ROM_ADDR(p) ((unsigned int)(uintptr_t)(p))

#if defined(__cplusplus) && !defined(PORT_PTR32_ALIAS)
extern "C++" {

/* With PORT_PTR_CHECK, storing a pointer outside the game's memory is fatal. The window ends at port_win_end
   (gen/ram.S): port_ram, port_hw and the host objects that the game's memory points to. */
#ifdef PORT_PTR_CHECK
#define PORT_PTR_CHECK_ENCODE(p)                                                                                    \
    do {                                                                                                            \
        if ((p) && ((const unsigned char *)(p) < port_ram || (const unsigned char *)(p) >= port_win_end)) {        \
            port_fatal("Ptr32 encode %p from %p", (const void *)(p), __builtin_return_address(0));                 \
        }                                                                                                           \
    } while (0)
#else
#define PORT_PTR_CHECK_ENCODE(p) ((void)0)
#endif

#define PORT_INLINE inline __attribute__((always_inline))

template <class T> struct Ptr32 {
    unsigned int w;

    Ptr32() = default;
#ifdef PORT_PTR_AUDIT
    /* The audit compile (docs/port64.md) makes a Ptr32 that is passed through '...' a warning. */
    PORT_INLINE Ptr32(const Ptr32 &o) : w(o.w) {}
    Ptr32 &operator=(const Ptr32 &o) = default;
#endif
    /* Explicit: in 'c ? ptr32 : p' the Ptr32 converts to T *, not the other way. An assignment stores. */
    PORT_INLINE explicit Ptr32(T *p) { w = encode(p); }
    PORT_INLINE Ptr32 &operator=(T *p) {
        w = encode(p);
        return *this;
    }

    PORT_INLINE static unsigned int encode(T *p) {
        PORT_PTR_CHECK_ENCODE(p);
        return PORT_P2W(p);
    }
    PORT_INLINE T *get() const { return PORT_W2P(T, w); }

    PORT_INLINE operator T *() const { return get(); }
    PORT_INLINE T *operator->() const {
#ifdef PORT_PTR_CHECK
        /* With a delta, '->' through NULL gives an address near the delta, not near 0, and need not fault. */
        if (!w) {
            port_fatal("Ptr32: '->' through NULL from %p", __builtin_return_address(0));
        }
#endif
        return (T *)((uintptr_t)w + PORT_PTR_DELTA);
    }
    template <class U = T> PORT_INLINE U &operator*() const { return *get(); }
    /* The index's own type makes this an exact match. With an int parameter, an index of another type (a pointer
       difference) is ambiguous with the built-in [] of the conversion to T *: GCC takes this one, clang refuses. */
    template <class U = T, class I> PORT_INLINE U &operator[](I i) const { return get()[(int)i]; }
    template <class U> PORT_INLINE explicit operator U *() const { return (U *)get(); }
    PORT_INLINE explicit operator bool() const { return w != 0; }
    /* (u32)p and the other casts to an integer give the word. */
#ifndef PORT_PTR_AUDIT
    PORT_INLINE explicit operator unsigned int() const { return w; }
    PORT_INLINE explicit operator int() const { return (int)w; }
    PORT_INLINE explicit operator unsigned long() const { return w; }
    PORT_INLINE explicit operator long() const { return (long)(int)w; }
    PORT_INLINE explicit operator unsigned short() const { return (unsigned short)w; }
#endif

    /* Steps of the pointer move the word by whole elements. */
    template <class U = T> PORT_INLINE Ptr32 &operator+=(int n) {
        w += (unsigned int)(n * (int)sizeof(U));
        return *this;
    }
    template <class U = T> PORT_INLINE Ptr32 &operator-=(int n) {
        w -= (unsigned int)(n * (int)sizeof(U));
        return *this;
    }
    template <class U = T> PORT_INLINE Ptr32 &operator++() {
        w += sizeof(U);
        return *this;
    }
    template <class U = T> PORT_INLINE Ptr32 &operator--() {
        w -= sizeof(U);
        return *this;
    }
    template <class U = T> PORT_INLINE T *operator++(int) {
        T *old = get();
        w += sizeof(U);
        return old;
    }
    template <class U = T> PORT_INLINE T *operator--(int) {
        T *old = get();
        w -= sizeof(U);
        return old;
    }

    PORT_INLINE unsigned int raw() const { return w; }
    PORT_INLINE void setRaw(unsigned int v) { w = v; }
    PORT_INLINE static Ptr32 fromRaw(unsigned int v) {
        Ptr32 p;
        p.w = v;
        return p;
    }
};

/* A pointer to a function in the game's memory. On the normal builds the word is the function's address. On an
   encoded build it is a handle into port_fn_table. */
template <class F> struct Fn32 {
    unsigned int w;

    Fn32() = default;
    PORT_INLINE explicit Fn32(F *f) { w = encode(f); }
    PORT_INLINE Fn32 &operator=(F *f) {
        w = encode(f);
        return *this;
    }
    PORT_INLINE static unsigned int encode(F *f) {
#ifdef PORT_PTR_ENCODED
        return f ? port_fn_word((PortFn)f) : 0;
#else
        return (unsigned int)(uintptr_t)f;
#endif
    }
    PORT_INLINE F *get() const {
#ifdef PORT_PTR_ENCODED
        unsigned int i = (w - PORT_FN_BASE) / 4;
        if (!w) {
            return 0;
        }
        if (w < PORT_FN_BASE || i >= port_fn_count + port_audio_fn_count) {
            port_fatal("Fn32: bad function word %08x", w);
        }
        return (F *)(i < port_fn_count ? port_fn_table[i] : port_audio_fn_table[i - port_fn_count]);
#else
        return (F *)(uintptr_t)w;
#endif
    }
    /* A call through the object converts it to the function pointer first. */
    PORT_INLINE operator F *() const { return get(); }
    PORT_INLINE explicit operator bool() const { return w != 0; }
    PORT_INLINE explicit operator unsigned int() const { return w; }
    PORT_INLINE explicit operator unsigned long() const { return w; }
    PORT_INLINE explicit operator int() const { return (int)w; }
    PORT_INLINE explicit operator long() const { return (long)(int)w; }
    PORT_INLINE unsigned int raw() const { return w; }
};

/* A message slot (OSMesg). A message is a number or a pointer. On an encoded build the word holds a pointer into
   the game's memory as its Ptr32 word and any other value as it is. */
#ifdef PORT_PTR_ENCODED
PORT_INLINE unsigned int port_mesg_encode(const void *p) {
    uintptr_t v = (uintptr_t)p;

    if ((const unsigned char *)p >= port_ram && (const unsigned char *)p < port_win_end) {
        return (unsigned int)(v - PORT_PTR_DELTA);
    }
    if (v > 0xFFFFFFFFu || (v >= PORT_PTR_BASE && v < PORT_PTR_BASE + (uintptr_t)(port_win_end - port_ram))) {
        port_fatal("PortMesg: the message %p is neither a number nor in the game's memory", p);
    }
    return (unsigned int)v;
}
PORT_INLINE void *port_mesg_decode(unsigned int w) {
    if (w >= PORT_PTR_BASE && w - PORT_PTR_BASE < (uintptr_t)(port_win_end - port_ram)) {
        return (void *)((uintptr_t)w + PORT_PTR_DELTA);
    }
    return (void *)(uintptr_t)w;
}
#else
#define port_mesg_encode(p) ((unsigned int)(uintptr_t)(p))
#define port_mesg_decode(w) ((void *)(uintptr_t)(w))
#endif

struct PortMesg {
    unsigned int w;

    PortMesg() = default;
    PORT_INLINE explicit PortMesg(unsigned int v) { w = v; }
    PORT_INLINE explicit PortMesg(int v) { w = (unsigned int)v; }
    PORT_INLINE explicit PortMesg(unsigned long v) { w = (unsigned int)v; }
    PORT_INLINE explicit PortMesg(long v) { w = (unsigned int)v; }
    template <class T> PORT_INLINE explicit PortMesg(T *p) { w = port_mesg_encode(p); }
    template <class T> PORT_INLINE PortMesg &operator=(T *p) {
        w = port_mesg_encode(p);
        return *this;
    }
    PORT_INLINE operator void *() const { return port_mesg_decode(w); }
    PORT_INLINE explicit operator unsigned int() const { return w; }
    PORT_INLINE explicit operator int() const { return (int)w; }
    PORT_INLINE explicit operator unsigned long() const { return w; }
    PORT_INLINE explicit operator long() const { return (long)(int)w; }
    template <class U> PORT_INLINE explicit operator U *() const { return (U *)port_mesg_decode(w); }
    PORT_INLINE unsigned int raw() const { return w; }
};

#define PORT_CHECK_WORD(type)                                                                                       \
    static_assert(sizeof(type) == 4 && alignof(type) == 4, #type " must be one 32-bit word");                     \
    static_assert(__is_trivially_copyable(type) && __is_standard_layout(type), #type " must copy as a word");
PORT_CHECK_WORD(Ptr32<void>)
PORT_CHECK_WORD(Ptr32<char>)
PORT_CHECK_WORD(Fn32<void(void)>)
PORT_CHECK_WORD(PortMesg)
#undef PORT_CHECK_WORD

/* A function that stores through a T ** gets PORT_OUT(T, &slot) for a Ptr32 slot, and a function that stores
   through a PTR32(T) * gets PORT_SLOT(T, &local) for a plain pointer. On the normal builds a Ptr32 is the
   pointer's own bits, and both are casts. On an encoded build the value goes through a temporary, which is
   written back at the end of the full expression that holds the call. */
#ifdef PORT_PTR_ENCODED
template <class T> struct PortOut {
    Ptr32<T> *slot;
    T *value;

    PORT_INLINE explicit PortOut(Ptr32<T> *s) : slot(s), value(*s) {}
    PORT_INLINE ~PortOut() { *slot = value; }
    PORT_INLINE operator T **() { return &value; }
};
template <class T> struct PortSlot {
    T **slot;
    Ptr32<T> value;

    PORT_INLINE explicit PortSlot(T **s) : slot(s) { value = *s; }
    PORT_INLINE ~PortSlot() { *slot = value; }
    PORT_INLINE operator Ptr32<T> *() { return &value; }
};
#define PORT_OUT(T, p) ((T **)PortOut<T>(p))
#define PORT_SLOT(T, p) ((Ptr32<T> *)PortSlot<T>(p))
#endif

/* The level parser's PORT_COPY reads a Ptr32 as a 4-byte unit (port_fixups.h). */
template <typename T> struct PortUnit;
template <class T> struct PortUnit<Ptr32<T>> {
    enum { value = 4 };
};
}
#endif
#else
/* The layout check (tools/check_layouts.py) compiles for the N64, where a pointer is a word. */
#define PORT_PTR_BASE ((unsigned int)port_ram)
#define PORT_P2W(p) ((unsigned int)(p))
#define PORT_W2P(T, w) ((T *)(w))
#endif /* PORT_TYPES_ONLY */

/* An address operand of a display-list command (gbi.h): a number as it is, a pointer as its word. */
#if defined(__cplusplus) && !defined(PORT_PTR32_ALIAS) && !defined(PORT_TYPES_ONLY)
extern "C++" {
template <class T> PORT_INLINE unsigned int port_gbi_addr(T *p) { return PORT_P2W(p); }
template <class T> PORT_INLINE unsigned int port_gbi_addr(const Ptr32<T> &p) { return p.w; }
PORT_INLINE unsigned int port_gbi_addr(int v) { return (unsigned int)v; }
PORT_INLINE unsigned int port_gbi_addr(unsigned int v) { return v; }
PORT_INLINE unsigned int port_gbi_addr(long v) { return (unsigned int)v; }
PORT_INLINE unsigned int port_gbi_addr(unsigned long v) { return (unsigned int)v; }
}
#define PORT_GBI_ADDR(x) port_gbi_addr(x)
#else
#define PORT_GBI_ADDR(x) ((unsigned int)(x))
#endif

#if !defined(PORT_OUT)
#define PORT_OUT(T, p) ((T **)(p))
#define PORT_SLOT(T, p) ((PTR32(T) *)(p))
#endif

#endif
