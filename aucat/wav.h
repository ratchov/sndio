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
#ifndef WAV_H
#define WAV_H

#include <sys/types.h>
#include "dsp.h"

struct wav {
	struct aparams par;		/* file params */
	int rate;			/* file sample rate */
	int nch;			/* file channel count */
#define HDR_AUTO	0
#define HDR_RAW		1
#define HDR_WAV		2
	int hdr;			/* header type */
	int fd;				/* file descriptor */
#define WAV_FREAD	1		/* open for reading */
#define WAV_FWRITE	2		/* open for writing */
	int flags;			/* bitmap of above */
	off_t curpos;			/* read/write position (bytes) */
	off_t startpos;			/* where payload starts */
	off_t endpos;			/* where payload ends */
	off_t maxpos;			/* max allowed pos (.wav limitation) */
	short *map;			/* mulaw/alaw conversions */
	char *path;			/* file name (debug only) */
};

int wav_open(struct wav *, char *, int, int, struct aparams *, int, int);
size_t wav_read(struct wav *, void *, size_t);
size_t wav_write(struct wav *, void *, size_t);
int wav_seek(struct wav *, off_t);
void wav_close(struct wav *);

#endif /* !defined(WAV_H) */
