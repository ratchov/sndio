/*	$OpenBSD$	*/
/*
 * Copyright (c) 2008-2012 Alexandre Ratchov <alex@caoua.org>
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
#include "abuf.h"
#include "utils.h"

void
abuf_init(struct abuf *buf, unsigned int len)
{
	buf->data = xmalloc(len);
	buf->len = len;
	buf->used = 0;
	buf->start = 0;
}

void
abuf_done(struct abuf *buf)
{
#ifdef DEBUG
	if (buf->used > 0)
		logx(3, "deleting non-empty buffer, used = %d", buf->used);
#endif
	xfree(buf->data);
	buf->data = (void *)0xdeadbeef;
}

/*
 * return the reader pointer and the number of bytes available
 */
unsigned char *
abuf_rgetblk(struct abuf *buf, int *rsize)
{
	int count;

	count = buf->len - buf->start;
	if (count > buf->used)
		count = buf->used;
	*rsize = count;
	return buf->data + buf->start;
}

/*
 * discard "count" bytes at the start position.
 */
void
abuf_rdiscard(struct abuf *buf, int count)
{
#ifdef DEBUG
	if (count < 0 || count > buf->used) {
		logx(0, "%s: bad count = %d", __func__, count);
		panic();
	}
#endif
	buf->used -= count;
	buf->start += count;
	if (buf->start >= buf->len)
		buf->start -= buf->len;
}

/*
 * advance the writer pointer by "count" bytes
 */
void
abuf_wcommit(struct abuf *buf, int count)
{
#ifdef DEBUG
	if (count < 0 || count > (buf->len - buf->used)) {
		logx(0, "%s: bad count = %d", __func__, count);
		panic();
	}
#endif
	buf->used += count;
}

/*
 * get writer pointer and the number of bytes writable
 */
unsigned char *
abuf_wgetblk(struct abuf *buf, int *rsize)
{
	int end, avail, count;

	end = buf->start + buf->used;
	if (end >= buf->len)
		end -= buf->len;
	avail = buf->len - buf->used;
	count = buf->len - end;
	if (count > avail)
		count = avail;
	*rsize = count;
	return buf->data + end;
}
