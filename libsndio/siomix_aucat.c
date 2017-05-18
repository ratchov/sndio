/*
 * Copyright (c) 2010-2011 Alexandre Ratchov <alex@caoua.org>
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
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sndio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "debug.h"
#include "aucat.h"
#include "siomix_priv.h"
#include "bsd-compat.h"

struct siomix_aucat_hdl {
	struct siomix_hdl siomix;
	struct aucat aucat;
	struct siomix_desc desc;
	struct amsg_mix_desc buf[16];
	size_t buf_wpos;
	int dump_wait;
};

static void siomix_aucat_close(struct siomix_hdl *);
static int siomix_aucat_nfds(struct siomix_hdl *);
static int siomix_aucat_pollfd(struct siomix_hdl *, struct pollfd *, int);
static int siomix_aucat_revents(struct siomix_hdl *, struct pollfd *);
static int siomix_aucat_setctl(struct siomix_hdl *, unsigned int, unsigned int);
static int siomix_aucat_onctl(struct siomix_hdl *);
static int siomix_aucat_ondesc(struct siomix_hdl *);

/*
 * operations every device should support
 */
struct siomix_ops siomix_aucat_ops = {
	siomix_aucat_close,
	siomix_aucat_nfds,
	siomix_aucat_pollfd,
	siomix_aucat_revents,
	siomix_aucat_setctl,
	siomix_aucat_onctl,
	siomix_aucat_ondesc
};

static int
siomix_aucat_rdata(struct siomix_aucat_hdl *hdl)
{
	struct siomix_desc desc;
	struct amsg_mix_desc *c;
	size_t rpos;
	int n;

	while (hdl->aucat.rstate == RSTATE_DATA) {

		/* read entries */
		while (hdl->buf_wpos < sizeof(hdl->buf) &&
		    hdl->aucat.rstate == RSTATE_DATA) {
			n = _aucat_rdata(&hdl->aucat,
			    (unsigned char *)hdl->buf + hdl->buf_wpos,
			    sizeof(hdl->buf) - hdl->buf_wpos,
			    &hdl->siomix.eof);
			if (n == 0 || hdl->siomix.eof)
				return 0;
			hdl->buf_wpos += n;
		}

		/* parse entries */
		c = hdl->buf;
		rpos = 0;
		while (rpos < hdl->buf_wpos) {
			strlcpy(desc.group, c->group, SIOMIX_NAMEMAX);
			strlcpy(desc.chan0.str, c->chan0.str, SIOMIX_NAMEMAX);
			desc.chan0.unit = (int16_t)ntohs(c->chan0.unit);
			strlcpy(desc.chan1.str, c->chan1.str, SIOMIX_NAMEMAX);
			desc.chan1.unit = (int16_t)ntohs(c->chan1.unit);
			strlcpy(desc.func, c->func, SIOMIX_NAMEMAX);
			desc.type = c->type;
			desc.addr = ntohs(c->addr);
			_siomix_ondesc_cb(&hdl->siomix,
			    &desc, ntohs(c->curval));
			rpos += sizeof(struct amsg_mix_desc);
			c++;
		}
		hdl->buf_wpos = 0;
	}
	return 1;
}

/*
 * execute the next message, return 0 if blocked
 */
static int
siomix_aucat_runmsg(struct siomix_aucat_hdl *hdl)
{
	if (!_aucat_rmsg(&hdl->aucat, &hdl->siomix.eof))
		return 0;
	switch (ntohl(hdl->aucat.rmsg.cmd)) {
	case AMSG_DATA:
		hdl->buf_wpos = 0;
		if (!siomix_aucat_rdata(hdl))
			return 0;
		break;
	case AMSG_MIXSET:
		DPRINTF("siomix_aucat_runmsg: got MIXSET\n");
		_siomix_onctl_cb(&hdl->siomix,
		    ntohs(hdl->aucat.rmsg.u.mixset.addr),
		    ntohs(hdl->aucat.rmsg.u.mixset.val));
		break;
	case AMSG_MIXSYNC:
		DPRINTF("siomix_aucat_runmsg: got MIXSYNC\n");
		hdl->dump_wait = 0;
		_siomix_ondesc_cb(&hdl->siomix, NULL, 0);
		break;
	default:
		DPRINTF("sio_aucat_runmsg: unhandled message %u\n",
		    hdl->aucat.rmsg.cmd);
		hdl->siomix.eof = 1;
		return 0;
	}
	hdl->aucat.rstate = RSTATE_MSG;
	hdl->aucat.rtodo = sizeof(struct amsg);
	return 1;
}

struct siomix_hdl *
_siomix_aucat_open(const char *str, unsigned int mode, int nbio)
{
	struct siomix_aucat_hdl *hdl;

	hdl = malloc(sizeof(struct siomix_aucat_hdl));
	if (hdl == NULL)
		return NULL;
	if (!_aucat_open(&hdl->aucat, str, mode)) {
		free(hdl);
		return NULL;
	}
	_siomix_create(&hdl->siomix, &siomix_aucat_ops, mode, nbio);
	hdl->dump_wait = 0;
	return (struct siomix_hdl *)hdl;
}

static void
siomix_aucat_close(struct siomix_hdl *addr)
{
	struct siomix_aucat_hdl *hdl = (struct siomix_aucat_hdl *)addr;

	_aucat_close(&hdl->aucat, hdl->siomix.eof);
	free(hdl);
}

static int
siomix_aucat_ondesc(struct siomix_hdl *addr)
{
	struct siomix_aucat_hdl *hdl = (struct siomix_aucat_hdl *)addr;

	while (hdl->aucat.wstate != WSTATE_IDLE) {
		if (!_siomix_psleep(&hdl->siomix, POLLOUT))
			return 0;
	}
	AMSG_INIT(&hdl->aucat.wmsg);
	hdl->aucat.wmsg.cmd = htonl(AMSG_MIXSUB);
	hdl->aucat.wmsg.u.mixsub.desc = 1;
	hdl->aucat.wmsg.u.mixsub.val = 0;
	hdl->aucat.wtodo = sizeof(struct amsg);
	if (!_aucat_wmsg(&hdl->aucat, &hdl->siomix.eof))
		return 0;
	hdl->dump_wait = 1;
	while (hdl->dump_wait) {
		DPRINTF("psleeping...\n");
		if (!_siomix_psleep(&hdl->siomix, 0))
			return 0;
		DPRINTF("psleeping done\n");
	}
	DPRINTF("done\n");
	return 1;
}

static int
siomix_aucat_onctl(struct siomix_hdl *addr)
{
	struct siomix_aucat_hdl *hdl = (struct siomix_aucat_hdl *)addr;

	while (hdl->aucat.wstate != WSTATE_IDLE) {
		if (!_siomix_psleep(&hdl->siomix, POLLOUT))
			return 0;
	}
	AMSG_INIT(&hdl->aucat.wmsg);
	hdl->aucat.wmsg.cmd = htonl(AMSG_MIXSUB);
	hdl->aucat.wmsg.u.mixsub.desc = 1;
	hdl->aucat.wmsg.u.mixsub.val = 1;
	hdl->aucat.wtodo = sizeof(struct amsg);
	if (!_aucat_wmsg(&hdl->aucat, &hdl->siomix.eof))
		return 0;
	return 1;
}

static int
siomix_aucat_setctl(struct siomix_hdl *addr, unsigned int a, unsigned int v)
{
	struct siomix_aucat_hdl *hdl = (struct siomix_aucat_hdl *)addr;

	hdl->aucat.wstate = WSTATE_MSG;
	hdl->aucat.wtodo = sizeof(struct amsg);
	hdl->aucat.wmsg.cmd = htonl(AMSG_MIXSET);
	hdl->aucat.wmsg.u.mixset.addr = htons(a);
	hdl->aucat.wmsg.u.mixset.val = htons(v);
	return _aucat_wmsg(&hdl->aucat, &hdl->siomix.eof);
}

static int
siomix_aucat_nfds(struct siomix_hdl *addr)
{
	return 1;
}

static int
siomix_aucat_pollfd(struct siomix_hdl *addr, struct pollfd *pfd, int events)
{
	struct siomix_aucat_hdl *hdl = (struct siomix_aucat_hdl *)addr;

	return _aucat_pollfd(&hdl->aucat, pfd, events | POLLIN);
}

static int
siomix_aucat_revents(struct siomix_hdl *addr, struct pollfd *pfd)
{
	struct siomix_aucat_hdl *hdl = (struct siomix_aucat_hdl *)addr;
	int revents;

	revents = _aucat_revents(&hdl->aucat, pfd);
	if (revents & POLLIN) {
		do {
			if (hdl->aucat.rstate == RSTATE_MSG) {
				if (!siomix_aucat_runmsg(hdl))
					break;
			}
			if (hdl->aucat.rstate == RSTATE_DATA) {
				if (!siomix_aucat_rdata(hdl))
					break;
			}
		} while (0);
		revents &= ~POLLIN;
	}
	if (hdl->siomix.eof)
		return POLLHUP;
	DPRINTFN(3, "siomix_aucat_revents: revents = 0x%x\n", revents);
	return revents;
}
