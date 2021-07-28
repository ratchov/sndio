#ifndef BSD_COMPAT_H
#define BSD_COMPAT_H

#ifdef USE_LIBBSD
#include <bsd/bsd.h>
#endif

#ifndef HAVE_GETPEEREID
#define getpeereid _sndio_getpeereid
int getpeereid(int, uid_t *, gid_t *);
#endif

#ifndef HAVE_ISSETUGID
#define issetugid _sndio_issetugid
int issetugid(void);
#endif

#ifndef HAVE_STRLCAT
#define strlcat _sndio_strlcat
size_t strlcat(char *, const char *, size_t);
#endif

#ifndef HAVE_STRLCPY
#define strlcpy _sndio_strlcpy
size_t strlcpy(char *, const char *, size_t);
#endif

#ifndef HAVE_STRTONUM
#define strtonum _sndio_strtonum
long long strtonum(const char *, long long, long long, const char **);
#endif

#ifndef HAVE_SOCK_CLOEXEC
#define strtonum _sndio_strtonum
long long strtonum(const char *, long long, long long, const char **);
#endif

#ifndef HAVE_CLOCK_GETTIME
#define CLOCK_MONOTONIC	0
#define clock_gettime _sndio_clock_gettime
struct timespec;
int clock_gettime(int, struct timespec *);
#endif

#ifndef HAVE_SOCK_CLOEXEC
#define SOCK_CLOEXEC	0
#endif

#if !defined(CLOCK_UPTIME) && defined(CLOCK_MONOTONIC)
#define CLOCK_UPTIME CLOCK_MONOTONIC
#endif

#endif /* !defined(BSD_COMPAT_H) */
