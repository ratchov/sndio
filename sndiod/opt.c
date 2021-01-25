/*	$OpenBSD$	*/
/*
 * Copyright (c) 2008-2011 Alexandre Ratchov <alex@caoua.org>
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
#include <string.h>

#include "dev.h"
#include "opt.h"
#include "utils.h"

struct opt *opt_list;

/*
 * create a new audio sub-device "configuration"
 */
struct opt *
opt_new(struct dev *d, char *name,
    int pmin, int pmax, int rmin, int rmax,
    int maxweight, int mmc, int dup, unsigned int mode)
{
	struct opt *o, **po;
	unsigned int len, num;
	char c;

	if (name == NULL) {
		name = d->ctl_name;
		len = strlen(name);
	} else {
		for (len = 0; name[len] != '\0'; len++) {
			if (len == OPT_NAMEMAX) {
				log_puts(name);
				log_puts(": too long\n");
				return NULL;
			}
			c = name[len];
			if ((c < 'a' || c > 'z') &&
			    (c < 'A' || c > 'Z')) {
				log_puts(name);
				log_puts(": only alphabetic chars allowed\n");
				return NULL;
			}
		}
	}

	num = 0;
	for (po = &opt_list; *po != NULL; po = &(*po)->next)
		num++;
	if (num >= OPT_NMAX) {
		log_puts(name);
		log_puts(": too many opts\n");
		return NULL;
	}

	if (opt_byname(name)) {
		log_puts(name);
		log_puts(": already defined\n");
		return NULL;
	}

	if (mmc) {
		if (mmc_dev != NULL && mmc_dev != d) {
			log_puts(name);
			log_puts(": MMC already setup for another device\n");
			return NULL;
		}
		mmc_dev = d;
		dev_log(mmc_dev);
		log_puts(": will be MMC controlled\n");
	}

	o = xmalloc(sizeof(struct opt));
	o->num = num;
	o->dev = d;
	if (mode & MODE_PLAY) {
		o->pmin = pmin;
		o->pmax = pmax;
	}
	if (mode & MODE_RECMASK) {
		o->rmin = rmin;
		o->rmax = rmax;
	}
	o->maxweight = maxweight;
	o->mmc = mmc;
	o->dup = dup;
	o->mode = mode;
	memcpy(o->name, name, len + 1);
	o->next = *po;
	*po = o;
	if (log_level >= 2) {
		dev_log(d);
		log_puts(".");
		log_puts(o->name);
		log_puts(":");
		if (o->mode & MODE_REC) {
			log_puts(" rec=");
			log_putu(o->rmin);
			log_puts(":");
			log_putu(o->rmax);
		}
		if (o->mode & MODE_PLAY) {
			log_puts(" play=");
			log_putu(o->pmin);
			log_puts(":");
			log_putu(o->pmax);
			log_puts(" vol=");
			log_putu(o->maxweight);
		}
		if (o->mode & MODE_MON) {
			log_puts(" mon=");
			log_putu(o->rmin);
			log_puts(":");
			log_putu(o->rmax);
		}
		if (o->mode & (MODE_RECMASK | MODE_PLAY)) {
			if (o->mmc)
				log_puts(" mmc");
			if (o->dup)
				log_puts(" dup");
		}
		log_puts("\n");
	}
	return o;
}

struct opt *
opt_byname(char *name)
{
	struct opt *o;

	for (o = opt_list; o != NULL; o = o->next) {
		if (strcmp(name, o->name) == 0)
			return o;
	}
	return NULL;
}

struct opt *
opt_bynum(int num)
{
	struct opt *o;

	for (o = opt_list; o != NULL; o = o->next) {
		if (o->num == num)
			return o;
	}
	return NULL;
}

void
opt_del(struct opt *o)
{
	struct opt **po;

	for (po = &opt_list; *po != o; po = &(*po)->next) {
#ifdef DEBUG
		if (*po == NULL) {
			log_puts("opt_del: not on list\n");
			panic();
		}
#endif
	}
	*po = o->next;
	xfree(o);
}

void
opt_init(struct opt *o)
{
	struct dev *d;

	if (strcmp(o->name, o->dev->ctl_name) != 0) {
		for (d = dev_list; d != NULL; d = d->next) {
			ctl_new(CTL_OPT_DEV, o, d,
			    CTL_SEL, "", o->name, -1, "device",
			    d->ctl_name, -1, 1, o->dev == d);
		}
	}
}

void
opt_done(struct opt *o)
{
	struct dev *d;

	for (d = dev_list; d != NULL; d = d->next)
		ctl_del(CTL_OPT_DEV, o, d);
}

/*
 * Set opt's device, and (if necessary) move clients to
 * to the new device
 */
void
opt_setdev(struct opt *o, struct dev *d)
{
	struct dev *odev;
	struct ctl *c;
	struct ctlslot *p;
	struct slot *s;
	int i;

	if (o->dev == d)
		return;

	/*
	 * if we're using MMC, move all opts to the new device, mmc_setdev()
	 * will call us back
	 */
	if (o->mmc && mmc_dev != d) {
		mmc_setdev(d);
		return;
	}

	c = ctl_find(CTL_OPT_DEV, o, o->dev);
	c->curval = 0;

	odev = o->dev;
	o->dev = d;

	c = ctl_find(CTL_OPT_DEV, o, o->dev);
	c->curval = 1;
	c->val_mask = ~0;

	for (i = 0; i < DEV_NSLOT; i++) {
		s = slot_array + i;
		if (s->opt == o)
			slot_setopt(s, o);
	}

	/* move controlling clients to new device */
	for (p = ctlslot_array, i = 0; i < DEV_NCTLSLOT; i++, p++) {
		if (p->ops == NULL)
			continue;
		if (p->opt == o) {
			p->dev_mask &= ~(1 << odev->num);
			dev_unref(odev);
			if (dev_ref(d))
				p->dev_mask |= (1 << d->num);
			ctlslot_update(p);
		}
	}
}

/*
 * Get a reference to opt's device
 */
struct dev *
opt_devref(struct opt *o)
{
	struct dev *d, *a;

	/* circulate to the first "alternate" device (greatest num) */
	for (a = o->dev; a->alt_next->num > a->num; a = a->alt_next)
		;

	/* find first working one */
	d = a;
	while (1) {
		if (dev_ref(d))
			break;
		d = d->alt_next;
		if (d == a)
			return NULL;
	}

	/* if device changed, move everything to the new one */
	if (d != o->dev)
		opt_setdev(o, d);

	return d;
}
