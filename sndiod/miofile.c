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

#include <sys/types.h>
#include <sys/time.h>

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sndio.h>
#include "defs.h"
#include "file.h"
#include "midi.h"
#include "miofile.h"
#include "utils.h"

struct miofile {
	struct mio_hdl *hdl;
	struct port *port;
	struct file *file;
};

int miofile_pollfd(void *, struct pollfd *);
int miofile_revents(void *, struct pollfd *);
void miofile_in(void *);
void miofile_out(void *);
void miofile_hup(void *);

struct fileops miofile_ops = {
	"mio",
	miofile_pollfd,
	miofile_revents,
	miofile_in,
	miofile_out,
	miofile_hup
};

struct miofile *
miofile_new(struct port *p)
{
	struct mio_hdl *hdl;
	struct miofile *f;

	hdl = mio_open(p->path, p->midi->mode, 1);
	if (hdl == NULL)
		return NULL;
	f = xmalloc(sizeof(struct miofile));
	f->port = p;
	f->hdl = hdl;
	f->file = file_new(&miofile_ops, f, p->path, mio_nfds(f->hdl));
	return f;
}

void
miofile_del(struct miofile *f)
{
	file_del(f->file);
	mio_close(f->hdl);
	xfree(f);
}

int
miofile_pollfd(void *addr, struct pollfd *pfd)
{
	struct miofile *f = addr;
	struct midi *ep = f->port->midi;
	int events = 0;

	if ((ep->mode & MODE_MIDIIN) && ep->ibuf.used < ep->ibuf.len)
		events |= POLLIN;
	if ((ep->mode & MODE_MIDIOUT) && ep->obuf.used > 0)
		events |= POLLOUT;
	return mio_pollfd(f->hdl, pfd, events);
}

int
miofile_revents(void *addr, struct pollfd *pfd)
{
	struct miofile *f = addr;

	return mio_revents(f->hdl, pfd);
}

void
miofile_in(void *arg)
{
	struct miofile *f = arg;
	struct midi *ep = f->port->midi;
	unsigned char *data;
	unsigned int n, count;

	for (;;) {
		data = abuf_wgetblk(&ep->ibuf, &count);
		if (count == 0)
			break;
		n = mio_read(f->hdl, data, count);
		if (n == 0)
			break;
		abuf_wcommit(&ep->ibuf, n);
		midi_in(ep);
		if (n < count)
			break;
	}
}

void
miofile_out(void *arg)
{
	struct miofile *f = arg;
	struct midi *ep = f->port->midi;
	unsigned char *data;
	unsigned int n, count;

	for (;;) {
		data = abuf_rgetblk(&ep->obuf, &count);
		if (count == 0)
			break;
		n = mio_write(f->hdl, data, count);
		if (n == 0)
			break;
		abuf_rdiscard(&ep->obuf, n);
		if (n < count)
			break;
	}
}

void
miofile_hup(void *arg)
{
	struct miofile *f = arg;

	port_close(f->port);
}
