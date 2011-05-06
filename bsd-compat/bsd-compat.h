#ifndef __packed
#define __packed	__attribute__((packed))
#endif

#ifdef COMPAT_LETOH
#include <byteswap.h>
#include <endian.h>
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define htole16(x) (x)
#define htole32(x) (x)
#define letoh16(x) (x)
#define letoh32(x) (x)
#else
#define htole16(x) bswap_16(x)
#define htole32(x) bswap_32(x)
#define letoh16(x) bswap_16(x)
#define letoh32(x) bswap_32(x)
#endif
#endif

int issetugid(void);
size_t strlcpy(char *, const char *, size_t);
size_t strlcat(char *, const char *, size_t);
long long strtonum(const char *, long long, long long, const char **);
