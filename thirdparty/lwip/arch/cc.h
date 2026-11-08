#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <string.h>
#include <limits.h>

#if defined(_WIN32) && !defined(__GNUC__)
#include <winsock2.h>
#else
#include <sys/time.h>
#endif

typedef uint8_t     u8_t;
typedef int8_t      s8_t;
typedef uint16_t    u16_t;
typedef int16_t     s16_t;
typedef uint32_t    u32_t;
typedef int32_t     s32_t;

typedef uintptr_t   mem_ptr_t;

#define LWIP_ERR_T  int

#if defined(_WIN32) && !defined(__GNUC__)
/* MSVC: lwIP's PACK_STRUCT_* triple is used as
 *     PACK_STRUCT_BEGIN struct foo {...} PACK_STRUCT_STRUCT; PACK_STRUCT_END
 * so the packing has to come from the pragma pair, not from the trailing attribute. */
#define PACK_STRUCT_BEGIN __pragma(pack(push, 1))
#define PACK_STRUCT_STRUCT
#define PACK_STRUCT_END __pragma(pack(pop))
#else
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT  __attribute__((packed))
#define PACK_STRUCT_END
#endif

#define LWIP_PLATFORM_DIAG(x)

#if defined(_WIN32) && !defined(__GNUC__)
__declspec(noreturn) void abort(void);
#else
void abort(void) __attribute__((__noreturn__));
#endif

#define LWIP_PLATFORM_ASSERT(x) \
    do { abort(); } while (0)

#ifndef BYTE_ORDER
#define BYTE_ORDER  LITTLE_ENDIAN
#endif

#define LWIP_PROVIDE_ERRNO

unsigned int lwip_port_rand(void);
#define LWIP_RAND()     ((u32_t)lwip_port_rand())

#endif
