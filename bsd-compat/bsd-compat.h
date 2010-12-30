#ifdef COMPAT_PACKED
#define __packed	__attribute__((packed))
#endif

#ifdef COMPAT_LETOH
#include <byteswap.h>
#include <endian.h>
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define letoh16(x) (x)
#define letoh32(x) (x)
#define htole16(x) (x)
#define htole32(x) (x)
#define ntohl(x)   bswap_32(x)
#else
#define letoh16(x) bswap_16(x)
#define letoh32(x) bswap_32(x)
#define htole16(x) bswap_16(x)
#define htole32(x) bswap_32(x)
#define ntohl(x)   (x)
#endif
#endif

/*
 * setgroups(2) is defined here, on linux
 */
#include <sys/types.h>
#include <grp.h>

/*
 * prototypes of these don't hurt
 */
long long strtonum(const char *, long long, long long, const char **);
size_t strlcpy(char *, const char *, size_t);
int issetugid(void);
int getpeereid(int, uid_t *, gid_t *);
