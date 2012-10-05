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

#include <sys/time.h>
#include <sys/types.h>

#include <poll.h>
#include <sndio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abuf.h"
#include "defs.h"
#include "dev.h"
#include "dsp.h"
#include "file.h"
#include "siofile.h"
#include "utils.h"

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
#define STATE_REC	0
#define STATE_CYCLE	1
#define STATE_PLAY	2
	int state;
};

int siofile_pollfd(void *, struct pollfd *);
int siofile_revents(void *, struct pollfd *);
void siofile_run(void *);
void siofile_hup(void *);

struct fileops siofile_ops = {
	"sio",
	siofile_pollfd,
	siofile_revents,
	siofile_run,
	siofile_run,
	siofile_hup
};

/*
 * print device name and state
 */
void
siofile_log(struct siofile *f)
{
	dev_log(f->dev);
}

void
siofile_onmove(void *arg, int delta)
{
	struct siofile *f = arg;

#ifdef DEBUG
	if (delta < 0 || delta > (60 * RATE_MAX)) {
		siofile_log(f);
		log_puts(": ");
		log_puti(delta);
		log_puts(": bogus sndio delta");
		panic();
	}
	if (log_level >= 4) {
		siofile_log(f);
		log_puts(": tick, delta = ");
		log_puti(delta);
		log_puts(", load = ");
		log_puti((file_utime - f->utime) / 1000);
		log_puts(" + ");
		log_puti((file_wtime - f->wtime) / 1000);
		log_puts("\n");
	}
	f->sum_utime += file_utime - f->utime;
	f->sum_wtime += file_wtime - f->wtime;
	f->wtime = file_wtime;
	f->utime = file_utime;
	if (f->dev->mode & MODE_PLAY)
		f->pused -= delta;
	if (f->dev->mode & MODE_REC)
		f->rused += delta;
#endif
	dev_onmove(f->dev, delta);
}

int
siofile_rec(struct siofile *f)
{
	struct dev *d = f->dev;
	unsigned char *data, *base;
	unsigned int n;

#ifdef DEBUG
	if (f->todo == 0) {
		log_puts("siofile_in: can't read data\n");
		panic();
	}
	if (d->prime > 0) {
		log_puts("siofile_in: unexpected data\n");
		panic();
	}
#endif
	base = d->decbuf ? d->decbuf : (unsigned char *)d->rbuf;
	data = base + d->rchan * d->round * d->par.bps - f->todo;
	n = sio_read(f->hdl, data, f->todo);
	f->todo -= n;
#ifdef DEBUG
	if (n == 0 && data == base && !sio_eof(f->hdl)) {
		siofile_log(f);
		log_puts(": read blocked at cycle start, sync error\n");
		/* don't panic since recording is slightly ahead of playback */
	}
	if (log_level >= 4) {
		siofile_log(f);
		log_puts(": read ");
		log_putu(n);
		log_puts(": bytes, todo ");
		log_putu(f->todo);
		log_puts("/");
		log_putu(d->round * d->rchan * d->par.bps);
		log_puts("\n");
	}
#endif
	if (f->todo > 0)
		return 0;
	return 1;
}

int
siofile_play(struct siofile *f)
{	
	struct dev *d = f->dev;
	unsigned char *data, *base;
	unsigned int n;

#ifdef DEBUG
	if (f->todo == 0) {
		log_puts("siofile_in: can't write data\n");
		panic();
	}
#endif
	base = d->encbuf ? d->encbuf : (unsigned char *)DEV_PBUF(d);
	data = base + d->pchan * d->round * d->par.bps - f->todo;
	n = sio_write(f->hdl, data, f->todo);
	f->todo -= n;
#ifdef DEBUG
	if (n == 0 && data == base && !sio_eof(f->hdl)) {
		siofile_log(f);
		log_puts(": write blocked at cycle start, sync error\n");
		/* don't panic since playback might be ahead of recording */
	}
	if (log_level >= 4) {
		siofile_log(f);
		log_puts(": wrote ");
		log_putu(n);
		log_puts(" bytes, todo ");
		log_putu(f->todo);
		log_puts("/");
		log_putu(d->round * d->pchan * d->par.bps);
		log_puts("\n");
	}
#endif
	if (f->todo > 0)
		return 0;
	return 1;
}

/*
 * open the device.
 */
struct siofile *
siofile_new(struct dev *d)
{
	struct sio_par par;
	struct sio_hdl *hdl;
	struct siofile *f;
	unsigned int mode = d->mode & (MODE_PLAY | MODE_REC);

	hdl = sio_open(d->path, mode, 1);
	if (hdl == NULL) {
		if (mode != (SIO_PLAY | SIO_REC))
			return NULL;
		hdl = sio_open(d->path, SIO_PLAY, 1);
		if (hdl != NULL)
			mode = SIO_PLAY;
		else {
			hdl = sio_open(d->path, SIO_REC, 1);
			if (hdl != NULL)
				mode = SIO_REC;
			else
				return NULL;
		}
		if (log_level >= 1) {
			log_puts("warning, device opened in ");
			log_puts(mode == SIO_PLAY ? "play-only" : "rec-only");
			log_puts(" mode\n");
		}
	}
	sio_initpar(&par);
	par.bits = d->par.bits;
	par.bps = d->par.bps;
	par.sig = d->par.sig;
	par.le = d->par.le;
	par.msb = d->par.msb;
	if (mode & SIO_PLAY)
		par.pchan = d->pchan;
	if (mode & SIO_REC)
		par.rchan = d->rchan;
	if (d->bufsz)
		par.appbufsz = d->bufsz;
	if (d->round)
		par.round = d->round;
	if (d->rate)
		par.rate = d->rate;
	if (!sio_setpar(hdl, &par))
		goto bad_close;
	if (!sio_getpar(hdl, &par))
		goto bad_close;
	d->par.bits = par.bits;
	d->par.bps = par.bps;
	d->par.sig = par.sig;
	d->par.le = par.le;
	d->par.msb = par.msb;
	if (mode & SIO_PLAY)
		d->pchan = par.pchan;
	if (mode & SIO_REC)
		d->rchan = par.rchan;
	f = xmalloc(sizeof(struct siofile));
	f->dev = d;
	f->hdl = hdl;
	d->bufsz = par.bufsz;
	d->round = par.round;
	d->rate = par.rate;
	if (!(mode & MODE_PLAY))
		d->mode &= ~(MODE_PLAY | MODE_MON);
	if (!(mode & MODE_REC))
		d->mode &= ~MODE_REC;
	sio_onmove(f->hdl, siofile_onmove, f);
	f->file = file_new(&siofile_ops, f, d->path, sio_nfds(f->hdl));
	return f;
 bad_close:
	sio_close(hdl);
	return NULL;
}

void
siofile_del(struct siofile *f)
{
#ifdef DEBUG
	if (log_level >= 3) {
		siofile_log(f);
		log_puts(": closed\n");
	}
#endif
	file_del(f->file);
	sio_close(f->hdl);
	free(f);
}

void
siofile_start(struct siofile *f)
{
	struct dev *d = f->dev;

	if (!sio_start(f->hdl)) {
		if (log_level >= 1) {
			siofile_log(f);
			log_puts(": failed to start device\n");
		}
		return;
	}
	if (d->mode & MODE_PLAY) {
		f->state = STATE_CYCLE;
		f->todo = 0;
	} else {
		f->state = STATE_REC;
		f->todo = d->round * d->rchan * d->par.bps;
	}
#ifdef DEBUG
	f->pused = 0;
	f->rused = 0;
	f->sum_utime = 0;
	f->sum_wtime = 0;
	f->wtime = file_wtime;
	f->utime = file_utime;
	if (log_level >= 3) {
		siofile_log(f);
		log_puts(": started\n");
	}
#endif
}

void
siofile_stop(struct siofile *f)
{
	if (!sio_eof(f->hdl) && !sio_stop(f->hdl)) {
		if (log_level >= 1) {
			siofile_log(f);
			log_puts(": failed to stop device\n");
		}
		return;
	}
#ifdef DEBUG
	if (log_level >= 3) {
		siofile_log(f);
		log_puts(": stopped, load avg = ");
		log_puti(f->sum_utime / 1000);
		log_puts(" / ");
		log_puti(f->sum_wtime / 1000);
		log_puts("\n");
	}
#endif
}

int
siofile_pollfd(void *arg, struct pollfd *pfd)
{
	struct siofile *f = arg;
	int events;
	
	events = (f->state == STATE_REC) ? POLLIN : POLLOUT;
	return sio_pollfd(f->hdl, pfd, events);
}

int
siofile_revents(void *arg, struct pollfd *pfd)
{
	struct siofile *f = arg;

	f->events = sio_revents(f->hdl, pfd);
	return f->events;
}

void
siofile_run(void *arg)
{
	struct siofile *f = arg;
	struct dev *d = f->dev;

	/*
	 * sio_read() and sio_write() would block at the end of the
	 * cycle so we *must* return and restart poll()'ing. Otherwise
	 * we may trigger dev_cycle() which would make all clients
	 * underrun (ex, on a play-only device)
	 */
	for (;;) {
		if (d->pstate != DEV_RUN)
			return;
		switch (f->state) {
		case STATE_REC:
#ifdef DEBUG
			if (!(f->events & POLLIN)) {
				siofile_log(f);
				log_puts(": recording, but POLLIN not set: ");
				log_putx(f->events);
				log_puts("\n");
				panic();
			}
			if (f->rused < d->round) {
				siofile_log(f);
				log_puts(": missed clock tick, rused = ");
				log_puti(f->rused);
				log_puts("/");
				log_puti(d->bufsz);
				log_puts("\n");
				panic();
			}
#endif
			if (!siofile_rec(f))
				return;
#ifdef DEBUG
			f->rused -= d->round;
			if (f->rused < 0) {
				/* rec buffer size is not known */
				siofile_log(f);
				log_puts(": out of bounds rused = ");
				log_puti(f->rused);
				log_puts("/");
				log_puti(d->bufsz);
				log_puts("\n");
				panic();
			}
			if (f->rused >= 2 * d->round) {
				siofile_log(f);
				log_puts(": rec hw xrun, rused = ");
				log_puti(f->rused);
				log_puts("/");
				log_puti(d->bufsz);
				log_puts("\n");
			}
#endif
			f->state = STATE_CYCLE;
			break;
		case STATE_CYCLE:
			dev_cycle(d);
			if (d->mode & MODE_PLAY) {
				f->state = STATE_PLAY;
				f->todo = d->round * d->pchan * d->par.bps;
				break;
			} else {
				f->state = STATE_REC;
				f->todo = d->round * d->rchan * d->par.bps;
				return;
			}
		case STATE_PLAY:
			if (!siofile_play(f))
				return;
#ifdef DEBUG
			f->pused += d->round;
			if (f->pused < 0 || f->pused > d->bufsz) {
				siofile_log(f);
				log_puts(": out of bounds pused = ");
				log_puti(f->pused);
				log_puts("/");
				log_puti(d->bufsz);
				log_puts("\n");
				panic();
			}
			if (f->pused <= d->bufsz - 2 * d->round) {
				siofile_log(f);
				log_puts(": play hw xrun, pused = ");
				log_puti(f->pused);
				log_puts("/");
				log_puti(d->bufsz);
				log_puts("\n");
			}
#endif
			d->poffs += d->round;
			if (d->poffs == d->bufsz)
				d->poffs = 0;
			if ((d->mode & MODE_REC) && d->prime == 0) {
				f->state = STATE_REC;
				f->todo = d->round * d->rchan * d->par.bps;
			} else
				f->state = STATE_CYCLE;
			return;
		}
	}
}

void
siofile_hup(void *arg)
{
	struct siofile *f = arg;
	struct dev *d = f->dev;

	dev_close(d);
}
