/*
 * Copyright (c) 2003-2007 Alexandre Ratchov <alex@caoua.org>
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
/*
 * log_xxx() routines are used to quickly store traces into a trace buffer.
 * This allows trances to be collected during time sensitive operations without
 * disturbing them. The buffer can be flushed on standard error later, when
 * slow syscalls are no longer disruptive, e.g. at the end of the poll() loop.
 */
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "utils.h"

/*
 * log buffer size
 */
#define LOG_BUFSZ	8192

/*
 * store a character in the log
 */
#define LOG_PUTC(c) do {			\
	if (log_used < LOG_BUFSZ)		\
		log_buf[log_used++] = (c);	\
} while (0)

char log_buf[LOG_BUFSZ];	/* buffer where traces are stored */
unsigned int log_used = 0;	/* bytes used in the buffer */
unsigned int log_sync = 1;	/* if true, flush after each '\n' */

/*
 * write the log buffer on stderr
 */
void
log_flush(void)
{
	if (log_used ==  0)
		return;
	write(STDERR_FILENO, log_buf, log_used);
	log_used = 0;
}

/*
 * store a string in the log
 */
void
log_puts(char *msg)
{
	char *p = msg;
	int c;

	while ((c = *p++) != '\0') {
		LOG_PUTC(c);
		if (log_sync && c == '\n')
			log_flush();
	}
}

/*
 * store a hex in the log
 */
void
log_putx(unsigned long num)
{
	char dig[sizeof(num) * 2], *p = dig, c;
	unsigned int ndig;

	if (num != 0) {
		for (ndig = 0; num != 0; ndig++) {
			*p++ = num & 0xf;
			num >>= 4;
		}
		for (; ndig != 0; ndig--) {
			c = *(--p);
			c += (c < 10) ? '0' : 'a' - 10;
			LOG_PUTC(c);
		}
	} else 
		LOG_PUTC('0');
}

/*
 * store a unsigned decimal in the log
 */
void
log_putu(unsigned long num)
{
	char dig[sizeof(num) * 3], *p = dig;
	unsigned int ndig;

	if (num != 0) {
		for (ndig = 0; num != 0; ndig++) {
			*p++ = num % 10;
			num /= 10;
		}
		for (; ndig != 0; ndig--)
			LOG_PUTC(*(--p) + '0');
	} else
		LOG_PUTC('0');
}

/*
 * store a signed decimal in the log
 */
void
log_puti(long num)
{
	if (num < 0) {
		LOG_PUTC('-');
		num = -num;
	}
	log_putu(num);
}

/*
 * abort program execution after a fatal error
 */
void
panic(void)
{
	log_flush();
	(void)kill(getpid(), SIGABRT);
	_exit(1);
}

/*
 * return a pseudo-random number
 */
unsigned
rnd(void)
{
	static unsigned seed = 1989123;

	seed = (seed * 1664525) + 1013904223;
	return seed;
}

void
memrnd(void *addr, size_t size)
{
	unsigned char *p = addr;
 
	while (size-- > 0)
		*(p++) = rnd() >> 24;
}

/*
 * header of a memory block
 */
struct xmalloc_hdr {
	struct xmalloc_hdr *next;	/* next allocated block */
	char *tag;			/* what the block is used for */
	size_t size;			/* data chunk size in bytes */
	char end[sizeof(void *)];	/* copy of trailer (random bytes) */
};

#define XMALLOC_HDR_SIZE	((sizeof(struct xmalloc_hdr) + 15) & ~15)

struct xmalloc_hdr *xmalloc_list = NULL;

/*
 * allocate 'size' bytes of memory (with size > 0). This functions never
 * fails (and never returns NULL), if there isn't enough memory then
 * we abort the program.  The memory block is randomized to break code
 * that doesn't initialize the block.  We also add a footer and a
 * trailer to detect writes outside the block boundaries.
 */
void *
xmalloc(size_t size, char *tag)
{
	struct xmalloc_hdr *hdr;
	char *p;
	
	if (size == 0) {
		log_puts(tag);
		log_puts(": xmalloc: nbytes = 0\n");
		panic();
	}
	hdr = malloc(size + XMALLOC_HDR_SIZE + sizeof(hdr->end));
	if (hdr == NULL) {
		log_puts(tag);
		log_puts(": xmalloc: failed to allocate ");
		log_putx(size);
		log_puts(" bytes\n");
		panic();
	}
	p = (char *)hdr + XMALLOC_HDR_SIZE;
	hdr->tag = tag;
	hdr->size = size;
	memrnd(hdr->end, sizeof(hdr->end));
	memset(p, 0xd0, size);
	memcpy(p + size, hdr->end, sizeof(hdr->end));	
	hdr->next = xmalloc_list;
	xmalloc_list = hdr;
	return p;
}

/*
 * free a memory block. Also check that the header and the trailer
 * weren't changed and randomise the block, so that the block is not
 * usable once freed
 */
void
xfree(void *p)
{
	struct xmalloc_hdr *hdr, **ph;

	hdr = (struct xmalloc_hdr *)((char *)p - XMALLOC_HDR_SIZE);
	if (memcmp(hdr->end, (char *)p + hdr->size, sizeof(hdr->end)) != 0) {
		log_puts(hdr->tag);
		log_puts(": block trailer corrupted\n");
		panic();
	}
	memset(p, 0xdf, hdr->size);
	for (ph = &xmalloc_list; *ph != NULL; ph = &(*ph)->next) {
		if (*ph == hdr) {
			*ph = hdr->next;
			free(hdr);
			return;
		}
	}
	log_puts(hdr->tag);
	log_puts(": not allocated (double free?)\n");
	panic();
}

void
xmalloc_exit(void)
{
	struct xmalloc_hdr *hdr;

	if (xmalloc_list) {
		log_puts("allocated memory blocs: ");
		for (hdr = xmalloc_list; hdr != NULL; hdr = hdr->next) {
			log_puts(hdr->tag);
			if (hdr->next)
				log_puts(", ");
		}
		log_puts("\n");
	}
}

char *
xstrdup(char *s, char *tag)
{
	size_t size;
	void *p;

	size = strlen(s) + 1;
	p = xmalloc(size, tag);
	memcpy(p, s, size);
	return p;
}
