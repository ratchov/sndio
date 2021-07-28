/*
 * Copyright (c) 2010 Jacob Meuser <jakemsr@sdf.lonestar.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#ifndef BSD_COMPAT_H
#define BSD_COMPAT_H

#include <sys/types.h>

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
