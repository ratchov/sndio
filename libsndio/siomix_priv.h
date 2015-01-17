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
#ifndef SIOMIX_PRIV_H
#define SIOMIX_PRIV_H

#include <sndio.h>

#define SIOMIX_MAXNFDS	4

/*
 * private ``handle'' structure
 */
struct siomix_hdl {
	struct siomix_ops *ops;
	void (*desc_cb)(void *, struct siomix_desc *, int);
	void *desc_arg;
	void (*ctl_cb)(void *, unsigned int, unsigned int);
	void *ctl_arg;
	unsigned int mode;		/* SIOMIX_READ | SIOMIX_WRITE */
	int nbio;			/* true if non-blocking io */
	int eof;			/* true if error occured */
};

/*
 * operations every device should support
 */
struct siomix_ops {
	void (*close)(struct siomix_hdl *);
	int (*nfds)(struct siomix_hdl *);
	int (*pollfd)(struct siomix_hdl *, struct pollfd *, int);
	int (*revents)(struct siomix_hdl *, struct pollfd *);
	int (*setctl)(struct siomix_hdl *, unsigned int, unsigned int);
	int (*onctl)(struct siomix_hdl *);
	int (*ondesc)(struct siomix_hdl *);
};

struct siomix_hdl *_siomix_aucat_open(const char *, unsigned int, int);
struct siomix_hdl *_siomix_obsd_open(const char *, unsigned int, int);
struct siomix_hdl *_siomix_fake_open(const char *, unsigned int, int);
#ifdef USE_SUN_MIXER
struct siomix_hdl *_siomix_sun_open(const char *, unsigned int, int);
#endif
#ifdef USE_ALSA_MIXER
struct siomix_hdl *_siomix_alsa_open(const char *, unsigned int, int);
#endif
void _siomix_create(struct siomix_hdl *,
    struct siomix_ops *, unsigned int, int);
void _siomix_ondesc_cb(struct siomix_hdl *,
    struct siomix_desc *, unsigned int);
void _siomix_onctl_cb(struct siomix_hdl *, unsigned int, unsigned int);
int _siomix_psleep(struct siomix_hdl *, int);

#endif /* !defined(SIOMIX_PRIV_H) */
