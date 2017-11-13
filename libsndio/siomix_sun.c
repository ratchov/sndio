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
 * the way the sun mixer is designed doesn't let us representing
 * it easily with the siomix api. For now expose only few
 * white-listed controls the same way as we do in kernel
 * for the wskbd volume keys.
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
#include "bsd-compat.h"

#define DEVPATH_PREFIX	"/dev/mixer"
#define DEVPATH_MAX 	(1 +		\
	sizeof(DEVPATH_PREFIX) - 1 +	\
	sizeof(int) * 3)

#define SUN_TO_SIOMIX(v) (((v) * 127 + 127) / 255)
#define SIOMIX_TO_SUN(v) (((v) * 255 + 63) / 127)

struct wskbd_vol
{
	int nch;			/* channels in the level control */
	int level_idx;			/* index of the level control */
	int level_val[8];		/* current value */
	int mute_idx;			/* index of the mute control */
	int mute_val;			/* per channel state of mute control */
	int base_addr;
	char *name;
};

struct siomix_sun_hdl {
	struct siomix_hdl siomix;
	struct wskbd_vol spkr, mic;
	int fd, events;
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

static int
initmute(struct siomix_sun_hdl *hdl, struct mixer_devinfo *info)
{
	struct mixer_devinfo mi;

	mi.index = info->next;
	for (mi.index = info->next; mi.index != -1; mi.index = mi.next) {
		if (ioctl(hdl->fd, AUDIO_MIXER_DEVINFO, &mi) < 0)
			break;
		if (strcmp(mi.label.name, AudioNmute) == 0)
			return mi.index;
	}
	return -1;
}

static int
initvol(struct siomix_sun_hdl *hdl, struct wskbd_vol *vol, char *cn, char *dn)
{
	struct mixer_devinfo dev, cls;

	for (dev.index = 0; ; dev.index++) {
		if (ioctl(hdl->fd, AUDIO_MIXER_DEVINFO, &dev) < 0)
			break;
		if (dev.type != AUDIO_MIXER_VALUE)
			continue;
		cls.index = dev.mixer_class;
		if (ioctl(hdl->fd, AUDIO_MIXER_DEVINFO, &cls) < 0)
			break;
		if (strcmp(cls.label.name, cn) == 0 &&
		    strcmp(dev.label.name, dn) == 0) {
			vol->nch = dev.un.v.num_channels;
			vol->level_idx = dev.index;
			vol->mute_idx = initmute(hdl, &dev);
			DPRINTF("using %s.%s, %d channels, %s\n", cn, dn,
			    vol->nch, vol->mute_idx >= 0 ? "mute" : "no mute");
			return 1;
		}
	}
	vol->level_idx = vol->mute_idx = -1;
	return 0;
}

static void
init(struct siomix_sun_hdl *hdl)
{
	static struct {
		char *cn, *dn;
	} spkr_names[] = {
		{AudioCoutputs, AudioNmaster},
		{AudioCinputs,  AudioNdac},
		{AudioCoutputs, AudioNdac},
		{AudioCoutputs, AudioNoutput}
	}, mic_names[] = {
		{AudioCrecord, AudioNrecord},
		{AudioCrecord, AudioNvolume},
		{AudioCinputs, AudioNrecord},
		{AudioCinputs, AudioNvolume},
		{AudioCinputs, AudioNinput}
	};
	int i;

	for (i = 0; i < sizeof(spkr_names) / sizeof(spkr_names[0]); i++) {
		if (initvol(hdl, &hdl->spkr,
			spkr_names[i].cn, spkr_names[i].dn)) {
			hdl->spkr.name = "spkr";
			hdl->spkr.base_addr = 0;
			break;
		}
	}
	for (i = 0; i < sizeof(mic_names) / sizeof(mic_names[0]); i++) {
		if (initvol(hdl, &hdl->mic,
			mic_names[i].cn, mic_names[i].dn)) {
			hdl->mic.name = "mic";
			hdl->mic.base_addr = 64;
			break;
		}
	}
}

static int
setvol(struct siomix_sun_hdl *hdl, struct wskbd_vol *vol, int addr, int val)
{
	struct mixer_ctrl ctrl;
	int i;

	addr -= vol->base_addr;
	if (vol->level_idx >= 0 && addr >= 0 && addr < vol->nch) {
		if (vol->level_val[addr] == val) {
			DPRINTF("level %d, no change\n", val);
			return 1;
		}
		vol->level_val[addr] = val;
		ctrl.dev = vol->level_idx;
		ctrl.type = AUDIO_MIXER_VALUE;
		ctrl.un.value.num_channels = vol->nch;
		for (i = 0; i < vol->nch; i++) {
			ctrl.un.value.level[i] =
			    SIOMIX_TO_SUN(vol->level_val[i]);
		}
		DPRINTF("vol %d setting to %d\n", addr, vol->level_val[addr]);
		if (ioctl(hdl->fd, AUDIO_MIXER_WRITE, &ctrl) < 0) {
			DPRINTF("level write failed\n");
			return 0;
		}
		_siomix_onctl_cb(&hdl->siomix, vol->base_addr + addr, val);
		return 1;
	}

	addr -= 32;
	if (vol->mute_idx >= 0 && addr >= 0 && addr < vol->nch) {
		val = val ? 1 : 0;
		if (vol->mute_val == val) {
			DPRINTF("mute %d, no change\n", val);
			return 1;
		}
		vol->mute_val = val;
		ctrl.dev = vol->mute_idx;
		ctrl.type = AUDIO_MIXER_ENUM;
		ctrl.un.ord = val;
		DPRINTF("mute setting to %d\n", val);
		if (ioctl(hdl->fd, AUDIO_MIXER_WRITE, &ctrl) < 0) {
			DPERROR("mute write\n");
			return 0;
		}
		for (i = 0; i < vol->nch; i++) {
			_siomix_onctl_cb(&hdl->siomix,
			    vol->base_addr + 32 + i, val);
		}
		return 1;
	}
	return 1;
}

static int
scanvol(struct siomix_sun_hdl *hdl, struct wskbd_vol *vol)
{
	struct siomix_desc desc;
	struct mixer_ctrl ctrl;
	int i, val;

	memset(&desc, 0, sizeof(struct siomix_desc));
	strlcpy(desc.group.str, "hw", sizeof(desc.group.str));
	desc.group.unit = -1;
	if (vol->level_idx >= 0) {
		ctrl.dev = vol->level_idx;
		ctrl.type = AUDIO_MIXER_VALUE;
		ctrl.un.value.num_channels = vol->nch;
		if (ioctl(hdl->fd, AUDIO_MIXER_READ, &ctrl) < 0) {
			DPRINTF("level read failed\n");
			return 0;
		}
		desc.type = SIOMIX_NUM;
		desc.chan1.str[0] = 0;
		desc.chan1.unit = -1;
		strlcpy(desc.func, "level", SIOMIX_NAMEMAX);
		strlcpy(desc.chan0.str, vol->name, SIOMIX_NAMEMAX);
		for (i = 0; i < vol->nch; i++) {
			desc.chan0.unit = i;
			desc.addr = vol->base_addr + i;
			val = SUN_TO_SIOMIX(ctrl.un.value.level[i]);
			vol->level_val[i] = val;
			_siomix_ondesc_cb(&hdl->siomix, &desc, val);
		}
	}
	if (vol->mute_idx >= 0) {
		ctrl.dev = vol->mute_idx;
		ctrl.type = AUDIO_MIXER_ENUM;
		if (ioctl(hdl->fd, AUDIO_MIXER_READ, &ctrl) < 0) {
			DPRINTF("mute read failed\n");
			return 0;
		}
		desc.type = SIOMIX_SW;
		desc.chan1.str[0] = 0;
		desc.chan1.unit = -1;
		strlcpy(desc.func, "mute", SIOMIX_NAMEMAX);
		strlcpy(desc.chan0.str, vol->name, SIOMIX_NAMEMAX);
		val = ctrl.un.ord ? 1 : 0;
		vol->mute_val = val;
		for (i = 0; i < vol->nch; i++) {
			desc.chan0.unit = i;
			desc.addr = vol->base_addr + 32 + i;
			_siomix_ondesc_cb(&hdl->siomix, &desc, val);
		}
	}
	return 1;
}

int
siomix_sun_getfd(const char *str, unsigned int mode, int nbio)
{
	const char *p;
	char path[DEVPATH_MAX];
	unsigned int devnum;
	int fd, flags;

#ifdef DEBUG
	_sndio_debug_init();
#endif
	p = _sndio_parsetype(str, "rsnd");
	if (p == NULL) {
		DPRINTF("siomix_sun_getfd: %s: \"rsnd\" expected\n", str);
		return -1;
	}
	switch (*p) {
	case '/':
		p++;
		break;
	default:
		DPRINTF("siomix_sun_getfd: %s: '/' expected\n", str);
		return -1;
	}
	p = _sndio_parsenum(p, &devnum, 255);
	if (p == NULL || *p != '\0') {
		DPRINTF("siomix_sun_getfd: %s: number expected after '/'\n", str);
		return -1;
	}
	snprintf(path, sizeof(path), DEVPATH_PREFIX "%u", devnum);
	if (mode == (SIOMIX_READ | SIOMIX_WRITE))
		flags = O_RDWR;
	else
		flags = (mode & SIOMIX_WRITE) ? O_WRONLY : O_RDONLY;
	while ((fd = open(path, flags | O_NONBLOCK | O_CLOEXEC)) < 0) {
		if (errno == EINTR)
			continue;
		DPERROR(path);
		return -1;
	}
	return fd;
}

struct siomix_hdl *
siomix_sun_fdopen(int fd, unsigned int mode, int nbio)
{
	struct siomix_sun_hdl *hdl;

#ifdef DEBUG
	_sndio_debug_init();
#endif
	hdl = malloc(sizeof(struct siomix_sun_hdl));
	if (hdl == NULL)
		return NULL;
	_siomix_create(&hdl->siomix, &siomix_sun_ops, mode, nbio);
	hdl->fd = fd;
	init(hdl);
	return (struct siomix_hdl *)hdl;
}

struct siomix_hdl *
_siomix_sun_open(const char *str, unsigned int mode, int nbio)
{
	struct siomix_hdl *hdl;
	int fd;

	fd = siomix_sun_getfd(str, mode, nbio);
	if (fd < 0)
		return NULL;
	hdl = siomix_sun_fdopen(fd, mode, nbio);
	if (hdl != NULL)
		return hdl;
	while (close(fd) < 0 && errno == EINTR)
		; /* retry */
	return NULL;
}

static void
siomix_sun_close(struct siomix_hdl *addr)
{
	struct siomix_sun_hdl *hdl = (struct siomix_sun_hdl *)addr;

	close(hdl->fd);
	free(hdl);
}

static int
siomix_sun_ondesc(struct siomix_hdl *addr)
{
	struct siomix_sun_hdl *hdl = (struct siomix_sun_hdl *)addr;

	if (!scanvol(hdl, &hdl->spkr) ||
	    !scanvol(hdl, &hdl->mic)) {
		hdl->siomix.eof = 1;
		return 0;
	}
	_siomix_ondesc_cb(&hdl->siomix, NULL, 0);
	return 1;
}

static int
siomix_sun_onctl(struct siomix_hdl *addr)
{
	return 1;
}

static int
siomix_sun_setctl(struct siomix_hdl *arg, unsigned int addr, unsigned int val)
{
	struct siomix_sun_hdl *hdl = (struct siomix_sun_hdl *)arg;

	if (!setvol(hdl, &hdl->spkr, addr, val) ||
	    !setvol(hdl, &hdl->mic, addr, val)) {
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
