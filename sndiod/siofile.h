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
#ifndef SIOFILE_H
#define SIOFILE_H

struct dev;

struct siofile {
	struct sio_hdl *hdl;
	unsigned int todo;
#ifdef DEBUG
	long long wtime, utime;
	long long sum_wtime, sum_utime;
	int pused, rused, events;
#endif
	struct dev *dev;
	struct file *file;
#define SIOFILE_READ	0
#define SIOFILE_CYCLE	1
#define SIOFILE_WRITE	2
	int cstate;
};

int siofile_open(struct siofile *, struct dev *);
void siofile_close(struct siofile *);
void siofile_log(struct siofile *);
void siofile_start(struct siofile *);
void siofile_stop(struct siofile *);

void siofile_read(struct siofile *, unsigned int);
void siofile_write(struct siofile *, unsigned int);

#endif /* !defined(SIOFILE_H) */
