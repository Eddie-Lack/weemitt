/* Declarations the game's sources need on the PC. port_pre.h includes this in every game file. */
#ifndef PORT_FIXUPS_H
#define PORT_FIXUPS_H

/* The level parser reads every field with CopyFromData. Its calls are written PORT_COPY, which also passes the size
   of the destination's scalar type, so that the field can be converted to host byte order. */
#ifdef __cplusplus
/* C++ has no _Generic. PortUnit<T> gives the unit of type T; a type without one is a compile error. */
extern "C++" {
struct Vec3;
struct Quat;
struct Box;
union NMEArg;
struct ObjectInstance;
template <typename T> struct PortUnit;
#define PORT_UNIT_OF(type, n)                                                                                       \
    template <> struct PortUnit<type> { enum { value = n }; };
PORT_UNIT_OF(char, 1) PORT_UNIT_OF(signed char, 1) PORT_UNIT_OF(unsigned char, 1) PORT_UNIT_OF(short, 2)
PORT_UNIT_OF(unsigned short, 2) PORT_UNIT_OF(int, 4) PORT_UNIT_OF(unsigned int, 4) PORT_UNIT_OF(long, 4)
PORT_UNIT_OF(unsigned long, 4) PORT_UNIT_OF(float, 4) PORT_UNIT_OF(Vec3, 4) PORT_UNIT_OF(Quat, 4)
PORT_UNIT_OF(NMEArg, 4) PORT_UNIT_OF(Box, 4) PORT_UNIT_OF(ObjectInstance *, 4)
#undef PORT_UNIT_OF
}
#define PORT_UNIT(x) PortUnit<__typeof__(x)>::value
#else
#define PORT_UNIT(x)                                                                                                \
    _Generic((x), char: 1, signed char: 1, unsigned char: 1, short: 2, unsigned short: 2, int: 4, unsigned int: 4,  \
             long: 4, unsigned long: 4, float: 4, Vec3: 4, Quat: 4, NMEArg: 4, Box: 4,                              \
             struct ObjectInstance *: 4, default: port_unit_unknown())
#endif
#define PORT_COPY(dst, src, size) port_copy_from_data((dst), (src), (size), PORT_UNIT(*(dst)))
/* Never defined: a destination of another type is a link error, to be added to PORT_UNIT. */
int port_unit_unknown(void);
void port_copy_from_data(void *dst, unsigned char **src, int size, int unit);
short port_read_token(unsigned char *data);

/* The game's messages (PRINTF) and fatal errors. */
void port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void port_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* The heap. On the N64 it ends at 0x80400000, the end of the 4 MB of RDRAM. The port's runs 16 MB past the end
   of its copy of RDRAM (port.h). The heap's addresses are Ptr32 words (ptr32.h). */
extern unsigned char port_ram[];
#define PORT_HEAP_TOP ((u32)PORT_PTR_BASE + 0x1800000)

/* What the next DMAMemory call loads (loaders.c converts banks to host byte order). */
#ifndef PORT_LOAD_KIND_DEFINED
#define PORT_LOAD_KIND_DEFINED
enum PortLoadKind {
    PORT_LOAD_RAW,
    PORT_LOAD_TEXTURE_BANK,
    PORT_LOAD_OBJECT_BANK,
};
#endif
extern int port_load_kind;

#endif
