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

#include <sys/param.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "debug.h"
#include "siomix_priv.h"
#include "bsd-compat.h"

struct siomix_hdl *
siomix_open(const char *str, unsigned int mode, int nbio)
{
	static char devany[] = SIOMIX_DEVANY;
	struct siomix_hdl *hdl;
	const char *p;

#ifdef DEBUG
	_sndio_debug_init();
#endif
	if (str == NULL) /* backward compat */
		str = devany;
	if (strcmp(str, devany) == 0 && !issetugid()) {
		str = getenv("AUDIODEVICE");
		if (str == NULL)
			str = devany;
	}
	if (strcmp(str, devany) == 0) {
		hdl = _siomix_aucat_open("/0", mode, nbio);
		if (hdl != NULL)
			return hdl;
#if defined(USE_SUN_MIXER)
		return _siomix_sun_open("/0", mode, nbio);
#elif defined(USE_ALSA_MIXER)
		return _siomix_alsa_open("/0", mode, nbio);
#else
		return NULL;
#endif
	}
	if ((p = _sndio_parsetype(str, "snd")) != NULL)
		return _siomix_aucat_open(p, mode, nbio);
#if defined(USE_ALSA_MIXER) || defined(USE_SUN_MIXER)
	if ((p = _sndio_parsetype(str, "rsnd")) != NULL) {
#if defined(USE_SUN_MIXER)
		return _siomix_sun_open(p, mode, nbio);
#elif defined(USE_ALSA_MIXER)
		return _siomix_alsa_open(p, mode, nbio);
#endif
	}
#endif
	DPRINTF("siomix_open: %s: unknown device type\n", str);
	return NULL;
}

void
_siomix_create(struct siomix_hdl *hdl, struct siomix_ops *ops,
    unsigned int mode, int nbio)
{
	hdl->ops = ops;
	hdl->mode = mode;
	hdl->nbio = nbio;
	hdl->eof = 0;
	hdl->ctl_cb = NULL;
}


int
_siomix_psleep(struct siomix_hdl *hdl, int event)
{
	struct pollfd pfds[SIOMIX_MAXNFDS];
	int revents, nfds;

	for (;;) {
		nfds = siomix_pollfd(hdl, pfds, event);
		if (nfds == 0)
			return 0;
		while (poll(pfds, nfds, -1) < 0) {
			if (errno == EINTR)
				continue;
			DPERROR("siomix_psleep: poll");
			hdl->eof = 1;
			return 0;
		}
		revents = siomix_revents(hdl, pfds);
		if (revents & POLLHUP) {
			DPRINTF("siomix_psleep: hang-up\n");
			return 0;
		}
		if (event == 0 || (revents & event))
			break;
	}
	return 1;
}

void
siomix_close(struct siomix_hdl *hdl)
{
	hdl->ops->close(hdl);
}

int
siomix_nfds(struct siomix_hdl *hdl)
{
	return hdl->ops->nfds(hdl);
}

int
siomix_pollfd(struct siomix_hdl *hdl, struct pollfd *pfd, int events)
{
	if (hdl->eof)
		return 0;
	return hdl->ops->pollfd(hdl, pfd, events);
}

int
siomix_revents(struct siomix_hdl *hdl, struct pollfd *pfd)
{
	if (hdl->eof)
		return POLLHUP;
	return hdl->ops->revents(hdl, pfd);
}

int
siomix_eof(struct siomix_hdl *hdl)
{
	return hdl->eof;
}

int
siomix_ondesc(struct siomix_hdl *hdl,
    void (*cb)(void *, struct siomix_desc *, int), void *arg)
{
	hdl->desc_cb = cb;
	hdl->desc_arg = arg;
	return hdl->ops->ondesc(hdl);
}

int
siomix_onctl(struct siomix_hdl *hdl,
    void (*cb)(void *, unsigned int, unsigned int), void *arg)
{
	hdl->ctl_cb = cb;
	hdl->ctl_arg = arg;
	return hdl->ops->onctl(hdl);
}

void
_siomix_ondesc_cb(struct siomix_hdl *hdl,
    struct siomix_desc *desc, unsigned int val)
{
	if (desc) {
		DPRINTF("%u -> %s[%u/%u].%s=%s[%u/%u]\n",
		    desc->addr,
		    desc->chan0.str, desc->chan0.min, desc->chan0.num,
		    desc->grp,
		    desc->chan1.str, desc->chan1.min, desc->chan1.num);
	}
	if (hdl->desc_cb)
		hdl->desc_cb(hdl->desc_arg, desc, val);
}

void
_siomix_onctl_cb(struct siomix_hdl *hdl, unsigned int addr, unsigned int val)
{
	if (hdl->ctl_cb)
		hdl->ctl_cb(hdl->ctl_arg, addr, val);
}

int
siomix_setctl(struct siomix_hdl *hdl, unsigned int addr, unsigned int val)
{
	if (!(hdl->mode & SIOMIX_WRITE))
		return 0;
	return hdl->ops->setctl(hdl, addr, val);
}
