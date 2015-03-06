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

/*
 *
 * TODO
 *	- fix ac97 based mixers
 *
 */

#ifdef USE_SUN_MIXER
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/audioio.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sndio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "debug.h"
#include "siomix_priv.h"

struct siomix_sun_hdl {
	struct siomix_hdl siomix;
	struct mixer_devinfo *info;
	struct mixer_ctrl *curval;
	int fd, ninfo, events;
	int iclass, oclass, rclass;	
};

static void siomix_sun_close(struct siomix_hdl *);
static int siomix_sun_nfds(struct siomix_hdl *);
static int siomix_sun_pollfd(struct siomix_hdl *, struct pollfd *, int);
static int siomix_sun_revents(struct siomix_hdl *, struct pollfd *);
static int siomix_sun_setctl(struct siomix_hdl *, unsigned int, unsigned int);
static int siomix_sun_onctl(struct siomix_hdl *);
static int siomix_sun_ondesc(struct siomix_hdl *);

/*
 * operations every device should support
 */
struct siomix_ops siomix_sun_ops = {
	siomix_sun_close,
	siomix_sun_nfds,
	siomix_sun_pollfd,
	siomix_sun_revents,
	siomix_sun_setctl,
	siomix_sun_onctl,
	siomix_sun_ondesc
};

struct siomix_hdl *
_siomix_sun_open(const char *str, unsigned int mode, int nbio)
{
	struct siomix_sun_hdl *hdl;
	int i, fd, flags;
	char path[PATH_MAX];
	struct mixer_devinfo mi;

	if (*str != '/') {
		DPRINTF("siomix_sun_open: %s: '/<devnum>' expected\n", str);
		return NULL;
	}
	str++;
	hdl = malloc(sizeof(struct siomix_sun_hdl));
	if (hdl == NULL)
		return NULL;
	_siomix_create(&hdl->siomix, &siomix_sun_ops, mode, nbio);
	snprintf(path, sizeof(path), "/dev/mixer%s", str);
	if (mode == (SIOMIX_READ | SIOMIX_WRITE))
		flags = O_RDWR;
	else
		flags = (mode & SIOMIX_WRITE) ? O_WRONLY : O_RDONLY;

	while ((fd = open(path, flags)) < 0) {
		if (errno == EINTR)
			continue;
		DPERROR(path);
		goto bad_free;
	}
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
		DPERROR("FD_CLOEXEC");
		goto bad_close;
	}
	hdl->iclass = hdl->oclass = hdl->rclass = -1;
	hdl->fd = fd;

	/*
	 * count the number of mixer knobs, and fetch the full mixer
	 * description
	 */
	for (mi.index = 0; ; mi.index++) {
		if (ioctl(fd, AUDIO_MIXER_DEVINFO, &mi) < 0)
			break;
	}
	hdl->ninfo = mi.index;
	hdl->info = malloc(hdl->ninfo * sizeof(struct mixer_devinfo));
	if (hdl->info == NULL) {
		DPERROR("malloc");
		goto bad_close;
	}
	hdl->curval = malloc(hdl->ninfo * sizeof(struct mixer_ctrl));
	if (hdl->curval == NULL) {
		DPERROR("malloc");
		goto bad_freeinfo;
	}
	for (i = 0; i < hdl->ninfo; i++) {
		hdl->info[i].index = i;
		if (ioctl(fd, AUDIO_MIXER_DEVINFO, &hdl->info[i]) < 0) {
			DPERROR("AUDIO_MIXER_DEVINFO");
			goto bad_freeval;
		}
	}
	return (struct siomix_hdl *)hdl;
bad_freeval:
	free(hdl->curval);
bad_freeinfo:
	free(hdl->info);
bad_close:
	close(fd);
bad_free:
	free(hdl);
	return NULL;
}

static void
siomix_sun_close(struct siomix_hdl *addr)
{
	struct siomix_sun_hdl *hdl = (struct siomix_sun_hdl *)addr;

	close(hdl->fd);
	free(hdl);
}

/*
 * parse foo-4:5 and convert it to "foo", 4, 2
 */
static int
copy_ch(struct siomix_sun_hdl *hdl,
    int icls, char *istr, char *ostr, int *rmin, int *rnum)
{
	size_t len;
	char *sep, *endp;
	long cmin, cmax;

	ostr[0] = 0;

	sep = strchr(istr, '-');
	if (sep) {
		len = sep - istr;
		if (len >= SIOMIX_NAMEMAX - 1)
			return 0;
		memcpy(ostr, istr, len);
		ostr[len] = 0;
		istr = sep + 1;

		cmin = strtol(istr, &endp, 10);
		if (endp != istr) {
			/*
			 * this a "foo-0:3" style range
			 */
			istr = endp;
			if (*endp == ':') {
				istr++;
				cmax = strtol(istr, &endp, 10);
				if (endp == istr) {
					DPRINTF("bad range\n");
					return 0;
				}
				istr = endp;
			} else if (*endp == 0) {
				cmax = cmin;
			} else {
				DPRINTF("unknown range\n");
				return 0;
			}
			*rmin = cmin;
			*rnum = cmax - cmin + 1;
		} else {
			if (strcmp(ostr, "line") == 0) {
				/* rename make "line-foo" to "foo" */
				ostr[0] = 0;
			} else {
				/* append unknown suffix with '_' */
				strlcat(ostr, "_", SIOMIX_NAMEMAX);
			}
			strlcat(ostr, istr, SIOMIX_NAMEMAX);
			*rmin = 0;
			*rnum = 0;
		}
	} else {
		*rmin = 0;
		*rnum = 0;
		strlcpy(ostr, istr, SIOMIX_NAMEMAX);
	}
	if (icls == -1)
		return 1;
	/*
	 * append "_in" and "_out" suffixes, as nowadays
	 * most jacks are bidirectional
	 */
	if (strcmp(ostr, "mic") == 0 ||
	    strcmp(ostr, "spkr") == 0 ||
	    strcmp(ostr, "hp") == 0 ||
	    strcmp(ostr, "line") == 0) {
		if (icls == hdl->iclass)
			strlcat(ostr, "_in", SIOMIX_NAMEMAX);
		if (icls == hdl->oclass)
			strlcat(ostr, "_out", SIOMIX_NAMEMAX);
	}

	/*
	 * record class may conflict with input/output
	 */
	if (icls == hdl->rclass) {
		if (strcmp(ostr, "volume") == 0)
			strlcpy(ostr, "rec", SIOMIX_NAMEMAX);
	}
	return 1;
}

static void
make_opt(char *opt, int min, int num)
{
	switch (num) {
	case 0:
		opt[0] = 0;
		break;
	case 1:
		snprintf(opt, SIOMIX_NAMEMAX, "%u", min);
		break;
	default:
		snprintf(opt, SIOMIX_NAMEMAX, "%u-%u", min, min + num - 1);
		break;
	}
}

static int
copyname_num(struct siomix_sun_hdl *hdl,
    struct mixer_devinfo *info, struct siomix_desc *desc, int *rmin, int *rnum)
{
	size_t len;
	char *sep, *istr;

	sep = strchr(info->label.name, '_');
	if (sep) {
		strlcpy(desc->grp, "mix", SIOMIX_NAMEMAX);
		desc->type = SIOMIX_VEC;
		len = sep - info->label.name;
		if (len >= SIOMIX_NAMEMAX - 1)
			return 0;
		memcpy(desc->chan0.str, info->label.name, len);
		desc->chan0.str[len] = 0;
		istr = sep + 1;
		if (!copy_ch(hdl, info->mixer_class, istr,
			desc->chan1.str, rmin, rnum))
			return 0;
	} else {
		strlcpy(desc->grp, "level", SIOMIX_NAMEMAX);
		desc->type = SIOMIX_NUM;
		istr = info->label.name;
		if (!copy_ch(hdl, info->mixer_class, istr,
			desc->chan0.str, rmin, rnum))
			return 0;
		desc->chan1.str[0] = '\0';
	}
	return 1;
}

static int
copyname_enum(struct siomix_sun_hdl *hdl,
    struct mixer_devinfo *info, struct siomix_desc *desc, int *rmin, int *rnum)
{
	char istr[SIOMIX_NAMEMAX], *sep;
	size_t len;
	
	sep = strrchr(info->label.name, '.');
	if (sep == NULL)
		sep = strrchr(info->label.name, '_');
	if (sep == NULL) {
		if (info->prev < 0) {
			fprintf(stderr, "no separator\n");
			return 0;
		}
		strlcpy(desc->grp, info->label.name, SIOMIX_NAMEMAX);
		while (info->prev >= 0)
			info = hdl->info + info->prev;
		if (!copy_ch(hdl, info->mixer_class, info->label.name,
			desc->chan0.str, rmin, rnum))
			return 0;
		desc->chan1.str[0] = 0;
	} else {
		strlcpy(desc->grp, sep + 1, SIOMIX_NAMEMAX);
		len = sep - info->label.name;
		if (len >= SIOMIX_NAMEMAX - 1)
			return 0;
		memcpy(istr, info->label.name, len);
		istr[len] = '\0';
		if (!copy_ch(hdl, info->mixer_class, istr,
			desc->chan0.str, rmin, rnum))
			return 0;
		desc->chan1.str[0] = 0;
	}
	/*
	 * certain cards expose adc[0-1].source and adc[2-3].source
	 * as different types, which we forbid.
	 */
	if (strcmp(desc->grp, "source") == 0) {
		if (info->type == AUDIO_MIXER_SET)
			strlcpy(desc->grp, "sources", SIOMIX_NAMEMAX);
	}
	return 1;
}

static int
ord_to_num(struct mixer_devinfo *info, int ord)
{
	int i;

	for (i = 0; i < info->un.e.num_mem; i++) {
		if (ord == info->un.e.member[i].ord)
			return i;
	}
	DPRINTF("mixer bug: order not found\n");
	return 0;
}

static int
mask_to_bit(struct mixer_devinfo *info, int index, int val)
{
	int mask;

	mask = info->un.s.member[index].mask;
	if ((mask & val) == mask)
		return 1;
	else
		return 0;
	return 0;
}

static int
enum_to_sw(struct mixer_devinfo *info, struct siomix_desc *desc)
{
	char *v0, *v1;

	if (info->un.e.num_mem != 2)
		return 0;
	v0 = info->un.e.member[ord_to_num(info, 0)].label.name;
	v1 = info->un.e.member[ord_to_num(info, 1)].label.name;
	if (strcmp(v0, "off") == 0 && strcmp(v1, "on") == 0)
		goto make_sw;
	if (strcmp(v0, "unplugged") == 0 && strcmp(v1, "plugged") == 0) {
		strlcpy(desc->grp,
	            info->un.e.member[1].label.name,
		    SIOMIX_NAMEMAX);
		goto make_sw;
	}
	return 0;
 make_sw:
	desc->chan1.str[0] = 0;
	desc->chan1.opt[0] = 0;
	desc->type = SIOMIX_SW;
	return 1;
}

static int
siomix_sun_ondesc(struct siomix_hdl *addr)
{
	struct siomix_sun_hdl *hdl = (struct siomix_sun_hdl *)addr;
	struct mixer_devinfo *info;
	struct mixer_ctrl *ctrl;
	struct siomix_desc desc;
	char *ostr;
	int i, j, v, cmin, cnum;

	for (i = 0; i < hdl->ninfo; i++) {
		info = hdl->info + i;
		ctrl = hdl->curval + i;
		ctrl->dev = i;
		ctrl->type = info->type;
		if (ctrl->type == AUDIO_MIXER_CLASS) {
			if (strcmp(info->label.name, "inputs") == 0)
				hdl->iclass = i;
			if (strcmp(info->label.name, "outputs") == 0)
				hdl->oclass = i;
			if (strcmp(info->label.name, "record") == 0)
				hdl->rclass = i;
			continue;
		}
		if (ctrl->type == AUDIO_MIXER_VALUE)
			ctrl->un.value.num_channels = info->un.v.num_channels;
		if (ioctl(hdl->fd, AUDIO_MIXER_READ, ctrl) < 0) {
			DPERROR("AUDIO_MIXER_READ");
			hdl->siomix.eof = 1;
			return 0;
		}		
	}

	info = hdl->info;
	for (i = 0; i < hdl->ninfo; i++) {
		DPRINTF("psarsing \"%s\"\n", info->label.name);
		switch (info->type) {
		case AUDIO_MIXER_VALUE:
			desc.addr = i * 32;
			if (!copyname_num(hdl, info, &desc, &cmin, &cnum))
				return 0;
			make_opt(desc.chan0.opt, cmin, cnum);
			desc.chan1.opt[0] = 0;
			for (j = 0; j < info->un.v.num_channels; j++) {
				v = hdl->curval[i].un.value.level[j] *
				    127 / 255;
				ostr = desc.type == SIOMIX_NUM ?
				    desc.chan0.opt : desc.chan1.opt;
				if (info->un.v.num_channels > 1) {
					make_opt(ostr, cmin + j, 1);
				} else {
					make_opt(ostr, cmin, cnum);
				}
				_siomix_ondesc_cb(&hdl->siomix, &desc, v);
				desc.addr++;
			}
			break;
		case AUDIO_MIXER_ENUM:
			desc.addr = i * 32;
			if (info->un.e.num_mem <= 1)
				break;
			if (!copyname_enum(hdl, info, &desc, &cmin, &cnum))
				return 0;
			if (enum_to_sw(info, &desc)) {
				make_opt(desc.chan0.opt, cmin, cnum);
				_siomix_ondesc_cb(&hdl->siomix, &desc,
				    ord_to_num(info, hdl->curval[i].un.ord));
				break;
			}
			for (j = 0; j < info->un.e.num_mem; j++) {
				if (!copyname_enum(hdl, info,
					&desc, &cmin, &cnum))
					return 0;
				make_opt(desc.chan0.opt, cmin, cnum);
				if (!copy_ch(hdl, info->mixer_class,
					info->un.e.member[j].label.name,
					desc.chan1.str, &cmin, &cnum))
					return 0;
				make_opt(desc.chan1.opt, cmin, cnum);
				desc.type = SIOMIX_LIST;
				v = (j == ord_to_num(info,
					 hdl->curval[i].un.ord)) ? 1 : 0;
				_siomix_ondesc_cb(&hdl->siomix, &desc, v);
				desc.addr++;
			}
			break;
		case AUDIO_MIXER_SET:
			desc.addr = i * 32;
			if (info->un.s.num_mem == 0)
				break;
			if (!copyname_enum(hdl, info, &desc, &cmin, &cnum))
				return 0;
			make_opt(desc.chan0.opt, cmin, cnum);
			desc.type = SIOMIX_LIST;
			for (j = 0; j < info->un.s.num_mem; j++) {
				if (!copy_ch(hdl, info->mixer_class,
					info->un.s.member[j].label.name,
					desc.chan1.str, &cmin, &cnum))
					return 0;
				make_opt(desc.chan1.opt, cmin, cnum);
				_siomix_ondesc_cb(&hdl->siomix, &desc,
				    mask_to_bit(info, j,
					hdl->curval[i].un.mask));
				desc.addr++;
			}
			break;
		}
		info++;
	}
	_siomix_ondesc_cb(&hdl->siomix, NULL, 0);
	return 1;
}

static int
siomix_sun_onctl(struct siomix_hdl *addr)
{
	//struct siomix_sun_hdl *hdl = (struct siomix_sun_hdl *)addr;

	return 1;
}

static int
siomix_sun_setctl(struct siomix_hdl *arg, unsigned int addr, unsigned int val)
{
	struct siomix_sun_hdl *hdl = (struct siomix_sun_hdl *)arg;
	struct mixer_ctrl *ctrl;
	struct mixer_devinfo *info;
	int base, offs, oldv;

	DPRINTF("siomix_sun_setctl: %d set to %d\n", addr, val);
	base = addr / 32;
	offs = addr % 32;
	ctrl = hdl->curval + base;
	info = hdl->info + base;

	switch (ctrl->type) {
	case AUDIO_MIXER_VALUE:
		oldv = ctrl->un.value.level[offs];
		ctrl->un.value.level[offs] = (val * 255 + 63) / 127;
		break;
	case AUDIO_MIXER_ENUM:
		if (val == 0)
			return 1;
		oldv = ord_to_num(info, ctrl->un.ord);
		if (oldv == offs)
			return 1;
		_siomix_onctl_cb(&hdl->siomix, 32 * base + oldv, 0);
		ctrl->un.ord = info->un.e.member[offs].ord;
		break;
	case AUDIO_MIXER_SET:
		if (val)
			ctrl->un.mask |= info->un.s.member[offs].mask;
		else
			ctrl->un.mask &= ~info->un.s.member[offs].mask;
		break;
	default:
		DPRINTF("siomix_sun_setctl: wrong addr %d\n", addr);
		hdl->siomix.eof = 1;
		return 0;
	}
	if (ioctl(hdl->fd, AUDIO_MIXER_WRITE, ctrl) < 0) {
		DPERROR("siomix_sun_setctl");
		hdl->siomix.eof = 1;
		return 0;
	}
	return 1;
}

static int
siomix_sun_nfds(struct siomix_hdl *addr)
{
	return 0;
}

static int
siomix_sun_pollfd(struct siomix_hdl *addr, struct pollfd *pfd, int events)
{
	struct siomix_sun_hdl *hdl = (struct siomix_sun_hdl *)addr;

	hdl->events = events;
	return 0;
}

static int
siomix_sun_revents(struct siomix_hdl *addr, struct pollfd *pfd)
{
	struct siomix_sun_hdl *hdl = (struct siomix_sun_hdl *)addr;

	return hdl->events & POLLOUT;
}
#endif
