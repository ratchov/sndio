/*	$OpenBSD$	*/
/*
 * Copyright (c) 2008 Alexandre Ratchov <alex@caoua.org>
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
 * Simple byte fifo.
 *
 * The abuf data is split in two parts: (1) valid data available to the reader
 * (2) space available to the writer, which is not necessarily unused. It works
 * as follows: the write starts filling at offset (start + used), once the data
 * is ready, the writer adds to used the count of bytes available.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abuf.h"
#include "defs.h"
#include "utils.h"

#ifdef DEBUG
void
abuf_log(struct abuf *buf)
{
	log_putu(buf->start);
	log_puts("+");
	log_putu(buf->used);
	log_puts("/");
	log_putu(buf->len);
}
#endif

void
abuf_init(struct abuf *buf, unsigned int len, unsigned int bpf)
{
	buf->data = xmalloc((size_t)len * bpf);
	buf->len = len;
	buf->bpf = bpf;
	buf->used = 0;
	buf->start = 0;
}

void
abuf_done(struct abuf *buf)
{
#ifdef DEBUG	
	if (buf->used > 0) {
		if (log_level >= 3) {
			log_puts("deleting non-empty buffer, used = ");
			log_putu(buf->used);
			log_puts("\n");
		}
	}
#endif
	xfree(buf->data);
	buf->data = (void *)0xdeadbeef;
}

/*
 * Get a pointer to the readable block
 */
unsigned char *
abuf_rgetblk(struct abuf *buf, unsigned int *rsize)
{
	unsigned int count;

	count = buf->len - buf->start;
	if (count > buf->used)
		count = buf->used;
	*rsize = count;
	return buf->data + buf->start * buf->bpf;
}

/*
 * Discard the block at the start postion.
 */
void
abuf_rdiscard(struct abuf *buf, unsigned int count)
{
#ifdef DEBUG
	if (count > buf->used) {
		log_puts("abuf_rdiscard: bad count = ");
		log_putu(count);
		log_puts("\n");
		panic();
	}
#endif
	buf->used -= count;
	buf->start += count;
	if (buf->start >= buf->len)
		buf->start -= buf->len;
}

/*
 * Commit the data written at the end postion.
 */
void
abuf_wcommit(struct abuf *buf, unsigned int count)
{
#ifdef DEBUG
	if (count > (buf->len - buf->used)) {
		log_puts("abuf_wcommit: bad count = ");
		log_putu(count);
		log_puts("\n");
		panic();
	}
#endif
	buf->used += count;
}

/*
 * Get a pointer to the writable block
 */
unsigned char *
abuf_wgetblk(struct abuf *buf, unsigned int *rsize)
{
	unsigned int end, avail, count;

	end = buf->start + buf->used;
	if (end >= buf->len)
		end -= buf->len;
	avail = buf->len - buf->used;
	count = buf->len - end;
	if (count > avail)
		count = avail;
	*rsize = count;
	return buf->data + end * buf->bpf;
}
