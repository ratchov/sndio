/*	$OpenBSD$	*/
/*
 * Copyright (c) 2014 Alexandre Ratchov <alex@caoua.org>
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
#include "dev_siomix.h"
#include "utils.h"
#include "bsd-compat.h"

void dev_siomix_ondesc(void *, struct siomix_desc *, int);
void dev_siomix_onctl(void *, unsigned int, unsigned int);
int dev_siomix_pollfd(void *, struct pollfd *);
int dev_siomix_revents(void *, struct pollfd *);
void dev_siomix_in(void *);
void dev_siomix_out(void *);
void dev_siomix_hup(void *);

struct fileops dev_siomix_ops = {
	"siomix",
	dev_siomix_pollfd,
	dev_siomix_revents,
	dev_siomix_in,
	dev_siomix_out,
	dev_siomix_hup
};

void
dev_siomix_ondesc(void *arg, struct siomix_desc *desc, int val)
{
	struct dev *d = arg;
	struct ctl *c;

	if (desc == NULL)
		return;

	/*
	 * if this control's name conflicts with
	 * our local controls, then rename ours
	 */
	for (c = d->ctl_list; c != NULL; c = c->next) {
		if (c->addr < d->ctl_addr ||
		    c->addr >= d->ctl_addr + CTLADDR_END)
			continue;
		if ((strcmp(c->chan0.str, desc->chan0.str) == 0 &&
		     c->chan0.unit == desc->chan0.unit) ||
		    (strcmp(c->chan0.str, desc->chan1.str) == 0 &&
		     c->chan0.unit == desc->chan1.unit)) {
			c->chan0.unit = dev_makeunit(d, c->chan0.str);
			c->desc_mask = ~0;
		}
	}

	dev_rmctl(d, desc->addr);
	dev_addctl(d, desc->type, desc->addr,
	    desc->chan0.str, desc->chan0.unit, desc->func,
	    desc->chan1.str, desc->chan1.unit, val);
}

void
dev_siomix_onctl(void *arg, unsigned int addr, unsigned int val)
{
	struct dev *d = arg;
	struct ctl *c;

	dev_log(d);
	log_puts(": onctl: addr = ");
	log_putu(addr);
	log_puts(", val = ");
	log_putu(val);
	log_puts("\n");

	for (c = d->ctl_list; c != NULL; c = c->next) {
		if (c->addr != addr)
			continue;
		ctl_log(c);
		log_puts(": new value -> ");
		log_putu(val);
		log_puts("\n");
		c->val_mask = ~0U;
		c->curval = val;
	}
}

/*
 * open the mixer device.
 */
void
dev_siomix_open(struct dev *d)
{
	d->siomix.hdl = siomix_open(d->path, SIOMIX_READ | SIOMIX_WRITE, 0);
	if (d->siomix.hdl == NULL)
		return;
	siomix_ondesc(d->siomix.hdl, dev_siomix_ondesc, d);
	siomix_onctl(d->siomix.hdl, dev_siomix_onctl, d);
	d->siomix.file = file_new(&dev_siomix_ops, d, d->path,
	    siomix_nfds(d->siomix.hdl));
}

/*
 * close the mixer device.
 */
void
dev_siomix_close(struct dev *d)
{
	if (d->siomix.hdl == NULL)
		return;
	file_del(d->siomix.file);
	siomix_close(d->siomix.hdl);
	d->siomix.hdl = NULL;
}

int
dev_siomix_pollfd(void *arg, struct pollfd *pfd)
{
	struct dev *d = arg;
	struct ctl *c;
	int events = 0;

	for (c = d->ctl_list; c != NULL; c = c->next) {
		if (c->dirty)
			events |= POLLOUT;
	}
	return siomix_pollfd(d->siomix.hdl, pfd, events);
}

int
dev_siomix_revents(void *arg, struct pollfd *pfd)
{
	struct dev *d = arg;

	return siomix_revents(d->siomix.hdl, pfd);
}

void
dev_siomix_in(void *arg)
{
}

void
dev_siomix_out(void *arg)
{
	struct dev *d = arg;
	struct ctl *c;
	int cnt;

	/*
	 * for each dirty ctl, call siomix_setctl() and dev_unref(). As
	 * dev_unref() may destroy the ctl_list, we must call it after
	 * we've finished iterating on it.
	 */
	cnt = 0;
	for (c = d->ctl_list; c != NULL; c = c->next) {
		if (!c->dirty)
			continue;
		if (!siomix_setctl(d->siomix.hdl, c->addr, c->curval)) {
			ctl_log(c);
			log_puts(": set failed\n");
			break;
		}
		if (log_level >= 2) {
			ctl_log(c);
			log_puts(": changed\n");
		}
		c->dirty = 0;
		cnt++;
	}
	while (cnt-- > 0)
		dev_unref(d);
}

void
dev_siomix_hup(void *arg)
{
	struct dev *d = arg;

	dev_siomix_close(d);
}
