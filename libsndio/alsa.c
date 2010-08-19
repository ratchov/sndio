/*	$OpenBSD: sun.c,v 1.37 2010/05/25 06:49:13 ratchov Exp $	*/
/*
 * Copyright (c) 2010 Jacob Meuser <jakemsr@sdf.lonestar.org>
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
/*
 * TODO:
 *
 * call hdl->cb_pos() from sun_read() and sun_write(), or better:
 * implement generic blocking sio_read() and sio_write() with poll(2)
 * and use non-blocking sio_ops only
 */
#ifdef USE_ALSA
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <values.h>

#include "sndio_priv.h"

struct alsa_hdl {
	struct sio_hdl sio;
	snd_pcm_t *out_pcm;
	snd_pcm_t *in_pcm;
	snd_pcm_hw_params_t *out_hwp;
	snd_pcm_sw_params_t *out_swp;
	snd_pcm_hw_params_t *in_hwp;
	snd_pcm_sw_params_t *in_swp;
	int filling;
	unsigned ibpf, obpf;		/* bytes per frame */
	unsigned ihfr, ohfr;		/* frames the hw transfered */
	unsigned isfr, osfr;		/* frames the sw transfered */
	unsigned ierr, oerr;		/* frames the hw dropped */
	int offset;			/* frames play is ahead of record */
	int idelta, odelta;		/* position reported to client */
	int infds, onfds;
};

static void alsa_close(struct sio_hdl *);
static int alsa_start(struct sio_hdl *);
static int alsa_stop(struct sio_hdl *);
static int alsa_setpar(struct sio_hdl *, struct sio_par *);
static int alsa_getpar(struct sio_hdl *, struct sio_par *);
static int alsa_getcap(struct sio_hdl *, struct sio_cap *);
static size_t alsa_read(struct sio_hdl *, void *, size_t);
static size_t alsa_write(struct sio_hdl *, const void *, size_t);
static int alsa_pollfd(struct sio_hdl *, struct pollfd *, int);
static int alsa_revents(struct sio_hdl *, struct pollfd *);
static int alsa_setvol(struct sio_hdl *, unsigned);
static void alsa_getvol(struct sio_hdl *);

static struct sio_ops alsa_ops = {
	alsa_close,
	alsa_setpar,
	alsa_getpar,
	alsa_getcap,
	alsa_write,
	alsa_read,
	alsa_start,
	alsa_stop,
	alsa_pollfd,
	alsa_revents,
	alsa_setvol,
	alsa_getvol
};

/*
 * convert ALSA format to sio_par encoding
 */
static int
alsa_fmttopar(struct alsa_hdl *hdl, snd_pcm_format_t fmt, struct sio_par *par)
{
	switch (fmt) {
	case SND_PCM_FORMAT_U8:
		par->bits = 8;
		par->sig = 0;
		break;
	case SND_PCM_FORMAT_S8:
		par->bits = 8;
		par->sig = 1;
		break;
	case SND_PCM_FORMAT_S16_LE:
		par->bits = 16;
		par->sig = 1;
		par->le = 1;
		break;
	case SND_PCM_FORMAT_S16_BE:
		par->bits = 16;
		par->sig = 1;
		par->le = 0;
		break;
	case SND_PCM_FORMAT_U16_LE:
		par->bits = 16;
		par->sig = 0;
		par->le = 1;
		break;
	case SND_PCM_FORMAT_U16_BE:
		par->bits = 16;
		par->sig = 0;
		par->le = 0;
		break;
	case SND_PCM_FORMAT_S24_LE:
		par->bits = 24;
		par->sig = 1;
		par->le = 1;
		break;
	case SND_PCM_FORMAT_S24_BE:
		par->bits = 24;
		par->sig = 1;
		par->le = 0;
		break;
	case SND_PCM_FORMAT_U24_LE:
		par->bits = 24;
		par->sig = 0;
		par->le = 1;
		break;
	case SND_PCM_FORMAT_U24_BE:
		par->bits = 24;
		par->sig = 0;
		par->le = 0;
		break;
	default:
		DPRINTF("alsa_fmttopar: unsupported format\n");
		hdl->sio.eof = 1;
		return 0;
	}
	par->msb = 1;
	par->bps = SIO_BPS(par->bits);

	return 1;
}


/*
 * convert sio_par encoding to ALSA format
 */
static void
alsa_enctofmt(struct alsa_hdl *hdl, snd_pcm_format_t *rfmt, struct sio_enc *enc)
{
	if (enc->bits == 8) {
		if (enc->sig == ~0U || !enc->sig)
			*rfmt = SND_PCM_FORMAT_U8;
		else
			*rfmt = SND_PCM_FORMAT_S8;
	} else if (enc->bits == 16) {
		if (enc->sig == ~0U || enc->sig) {
			if (enc->le == ~0U) {
				*rfmt = SIO_LE_NATIVE ?
				    SND_PCM_FORMAT_S16_LE :
				    SND_PCM_FORMAT_S16_BE;
			} else if (enc->le)
				*rfmt = SND_PCM_FORMAT_S16_LE;
			else
				*rfmt = SND_PCM_FORMAT_S16_BE;
		} else {
			if (enc->le == ~0U) {
				*rfmt = SIO_LE_NATIVE ?
				    SND_PCM_FORMAT_U16_LE :
				    SND_PCM_FORMAT_U16_BE;
			} else if (enc->le)
				*rfmt = SND_PCM_FORMAT_U16_LE;
			else
				*rfmt = SND_PCM_FORMAT_U16_BE;
		}
	} else if (enc->bits == 24) {
		if (enc->sig == ~0U || enc->sig) {
			if (enc->le == ~0U) {
				*rfmt = SIO_LE_NATIVE ?
				    SND_PCM_FORMAT_S24_LE :
				    SND_PCM_FORMAT_S24_BE;
			 } else if (enc->le)
				*rfmt = SND_PCM_FORMAT_S24_LE;
			else
				*rfmt = SND_PCM_FORMAT_S24_BE;
		} else {
			if (enc->le == ~0U) {
				*rfmt = SIO_LE_NATIVE ?
				    SND_PCM_FORMAT_U24_LE :
				    SND_PCM_FORMAT_U24_BE;
			} else if (enc->le)
				*rfmt = SND_PCM_FORMAT_U24_LE;
			else
				*rfmt = SND_PCM_FORMAT_U24_BE;
		}
	} else {
		/* XXX */
		*rfmt = SIO_LE_NATIVE ?
		    SND_PCM_FORMAT_S16_LE : SND_PCM_FORMAT_S16_BE;
	}
}

/*
 * try to set the device to the given parameters and check that the
 * device can use them; return 1 on success, 0 on failure or error
 */
#if 0
static int
alsa_tryinfo(struct alsa_hdl *hdl, struct sio_enc *enc,
    unsigned pchan, unsigned rchan, unsigned rate)
{
	snd_pcm_format_t fmt;

	alsa_enctofmt(hdl, &fmt, enc);
	if (hdl->sio.mode & SIO_PLAY) {
		if (snd_pcm_hw_params_test_format(hdl->out_pcm, hdl->out_hwp,
		    fmt) < 0)
			return 0;
	}
	if (hdl->sio.mode & SIO_REC) {
		if (snd_pcm_hw_params_test_format(hdl->in_pcm, hdl->in_hwp,
		    fmt) < 0)
			return 0;
	}

	if (pchan && (hdl->sio.mode & SIO_PLAY)) {
		if (snd_pcm_hw_params_test_channels(hdl->out_pcm, hdl->out_hwp,
		    pchan) < 0)
			return 0;
	}
	if (rchan && (hdl->sio.mode & SIO_REC)) {
		if (snd_pcm_hw_params_test_channels(hdl->in_pcm, hdl->in_hwp,
		    rchan) < 0)
			return 0;
	}

	if (rate && (hdl->sio.mode & SIO_PLAY)) {
		if (snd_pcm_hw_params_test_rate(hdl->out_pcm, hdl->out_hwp,
		    rate, 0) < 0)
			return 0;
	}
	if (rate && (hdl->sio.mode & SIO_REC)) {
		if (snd_pcm_hw_params_test_rate(hdl->in_pcm, hdl->in_hwp,
		    rate, 0) < 0)
			return 0;
	}

	return 1;
}
#endif

/*
 * guess device capabilities
 */
static int
alsa_getcap(struct sio_hdl *sh, struct sio_cap *cap)
{
#if 0
#define NCHANS (sizeof(chans) / sizeof(chans[0]))
#define NRATES (sizeof(rates) / sizeof(rates[0]))
	static unsigned chans[] = {
		1, 2, 4, 6, 8, 10, 12
	};
	static unsigned rates[] = {
		8000, 11025, 12000, 16000, 22050, 24000,
		32000, 44100, 48000, 64000, 88200, 96000
	};
	struct alsa_hdl *hdl = (struct alsa_hdl *)sh;
	struct sio_par savepar;
	unsigned nenc = 0, nconf = 0;
	unsigned enc_map = 0, rchan_map = 0, pchan_map = 0, rate_map;
	unsigned i, j, conf;
	unsigned fmt;

	if (!alsa_getpar(&hdl->sio, &savepar))
		return 0;

	/*
	 * fill encoding list
	 */
	for (ae.index = 0; nenc < SIO_NENC; ae.index++) {
		if (ioctl(hdl->fd, AUDIO_GETENC, &ae) < 0) {
			if (errno == EINVAL)
				break;
			DPERROR("alsa_getcap: getenc");
			hdl->sio.eof = 1;
			return 0;
		}
		if (ae.flags & AUDIO_ENCODINGFLAG_EMULATED)
			continue;
		if (ae.encoding == AUDIO_ENCODING_SLINEAR_LE) {
			cap->enc[nenc].le = 1;
			cap->enc[nenc].sig = 1;
		} else if (ae.encoding == AUDIO_ENCODING_SLINEAR_BE) {
			cap->enc[nenc].le = 0;
			cap->enc[nenc].sig = 1;
		} else if (ae.encoding == AUDIO_ENCODING_ULINEAR_LE) {
			cap->enc[nenc].le = 1;
			cap->enc[nenc].sig = 0;
		} else if (ae.encoding == AUDIO_ENCODING_ULINEAR_BE) {
			cap->enc[nenc].le = 0;
			cap->enc[nenc].sig = 0;
		} else if (ae.encoding == AUDIO_ENCODING_SLINEAR) {
			cap->enc[nenc].le = SIO_LE_NATIVE;
			cap->enc[nenc].sig = 1;
		} else if (ae.encoding == AUDIO_ENCODING_ULINEAR) {
			cap->enc[nenc].le = SIO_LE_NATIVE;
			cap->enc[nenc].sig = 0;
		} else {
			/* unsipported encoding */
			continue;
		}
		cap->enc[nenc].bits = ae.precision;
		cap->enc[nenc].bps = SIO_BPS(ae.precision);
		cap->enc[nenc].msb = 1;
		enc_map |= (1 << nenc);
		nenc++;
	}

	/*
	 * fill channels
	 *
	 * for now we're lucky: all kernel devices assume that the
	 * number of channels and the encoding are independent so we can
	 * use the current encoding and try various channels.
	 */
	if (hdl->sio.mode & SIO_PLAY) {
		memcpy(&cap->pchan, chans, NCHANS * sizeof(unsigned));
		for (i = 0; i < NCHANS; i++) {
			if (alsa_tryinfo(hdl, NULL, chans[i], 0, 0))
				pchan_map |= (1 << i);
		}
	}
	if (hdl->sio.mode & SIO_REC) {
		memcpy(&cap->rchan, chans, NCHANS * sizeof(unsigned));
		for (i = 0; i < NCHANS; i++) {
			if (alsa_tryinfo(hdl, NULL, 0, chans[i], 0))
				rchan_map |= (1 << i);
		}
	}

	/*
	 * fill rates
	 *
	 * rates are not independent from other parameters (eg. on
	 * uaudio devices), so certain rates may not be allowed with
	 * certain encodings. We have to check rates for all encodings
	 */
	memcpy(&cap->rate, rates, NRATES * sizeof(unsigned));
	for (j = 0; j < nenc; j++) {
		rate_map = 0;
		for (i = 0; i < NRATES; i++) {
			if (alsa_tryinfo(hdl, &cap->enc[j], 0, 0, rates[i]))
				rate_map |= (1 << i);
		}
		for (conf = 0; conf < nconf; conf++) {
			if (cap->confs[conf].rate == rate_map) {
				cap->confs[conf].enc |= (1 << j);
				break;
			}
		}
		if (conf == nconf) {
			if (nconf == SIO_NCONF)
				break;
			cap->confs[nconf].enc = (1 << j);
			cap->confs[nconf].pchan = pchan_map;
			cap->confs[nconf].rchan = rchan_map;
			cap->confs[nconf].rate = rate_map;
			nconf++;
		}
	}
	cap->nconf = nconf;
	if (!alsa_setpar(&hdl->sio, &savepar))
		return 0;
	return 1;
#undef NCHANS
#undef NRATES
#endif
	return 1;
}

static void
alsa_getvol(struct sio_hdl *sh)
{
	struct alsa_hdl *hdl = (struct alsa_hdl *)sh;

	sio_onvol_cb(&hdl->sio, SIO_MAXVOL);
}

int
alsa_setvol(struct sio_hdl *sh, unsigned vol)
{
	return 1;
}

struct sio_hdl *
sio_open_alsa(const char *str, unsigned mode, int nbio)
{
	struct alsa_hdl *hdl;
	char path[PATH_MAX];

	hdl = malloc(sizeof(struct alsa_hdl));
	if (hdl == NULL)
		return NULL;
	sio_create(&hdl->sio, &alsa_ops, mode, nbio);

	snprintf(path, sizeof(path), "hw:%s", str);

	if (mode & SIO_PLAY) {
		if (snd_pcm_open(&hdl->out_pcm, path, SND_PCM_STREAM_PLAYBACK,
		    SND_PCM_NONBLOCK) < 0) {
			DPERROR(path);
			goto bad_free;
		}
		snd_pcm_nonblock(hdl->out_pcm, 1);
	}

	if (mode & SIO_REC) {
		if (snd_pcm_open(&hdl->in_pcm, path, SND_PCM_STREAM_CAPTURE,
		    SND_PCM_NONBLOCK) < 0) {
			DPERROR(path);
			goto bad_free;
		}
		snd_pcm_nonblock(hdl->in_pcm, 1);
	}

	if (hdl->out_pcm) {
		if (snd_pcm_hw_params_malloc(&hdl->out_hwp) < 0) {
			DPERROR("could not alloc out_hwp");
			goto bad_free;
		}
		if (snd_pcm_sw_params_malloc(&hdl->out_swp) < 0) {
			DPERROR("could not alloc out_swp");
			goto bad_free;
		}
	}
	if (hdl->in_pcm) {
		if (snd_pcm_hw_params_malloc(&hdl->in_hwp) < 0) {
			DPERROR("could not alloc in_hwp");
			goto bad_free;
		}
		if (snd_pcm_sw_params_malloc(&hdl->in_swp) < 0) {
			DPERROR("could not alloc in_swp");
			goto bad_free;
		}
	}

	/*
	 * Default parameters may not be compatible with libsndio (eg. mulaw
	 * encodings, different playback and recording parameters, etc...), so
	 * set parameters to a random value. If the requested parameters are
	 * not supported by the device, then sio_setpar() will pick supported
	 * ones.
	 */

	if (mode & SIO_PLAY) {
		if (snd_pcm_hw_params_any(hdl->out_pcm, hdl->out_hwp) < 0) {
			DPERROR("no playback configurations available");
			goto bad_free;
		}
		if (snd_pcm_hw_params_set_access(hdl->out_pcm, hdl->out_hwp,
		    SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
			DPERROR("cannot use interleaved samples for playback");
			goto bad_free;
		}
	}
	if (mode & SIO_REC) {
		if (snd_pcm_hw_params_any(hdl->in_pcm, hdl->in_hwp) < 0) {
			DPERROR("no recording configurations available");
			goto bad_free;
		}
		if (snd_pcm_hw_params_set_access(hdl->in_pcm, hdl->in_hwp,
		    SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
			DPERROR("cannot use interleaved samples for recording");
			goto bad_free;
		}
	}

	return (struct sio_hdl *)hdl;
 bad_free:
	free(hdl);
	return NULL;
}

static void
alsa_close(struct sio_hdl *sh)
{
	struct alsa_hdl *hdl = (struct alsa_hdl *)sh;

	if (hdl->sio.mode & SIO_PLAY) {
		snd_pcm_close(hdl->out_pcm);
		snd_pcm_hw_params_free(hdl->out_hwp);
		snd_pcm_sw_params_free(hdl->out_swp);
	}
	if (hdl->sio.mode & SIO_REC) {
		snd_pcm_close(hdl->in_pcm);
		snd_pcm_hw_params_free(hdl->in_hwp);
		snd_pcm_sw_params_free(hdl->in_swp);
	}

	free(hdl);
}

static int
alsa_start(struct sio_hdl *sh)
{
	struct sio_par par;
	struct alsa_hdl *hdl = (struct alsa_hdl *)sh;

	if (!sio_getpar(&hdl->sio, &par))
		return 0;
	hdl->ibpf = par.rchan * par.bps;
	hdl->obpf = par.pchan * par.bps;
	hdl->ihfr = 0;
	hdl->ohfr = 0;
	hdl->isfr = 0;
	hdl->osfr = 0;
	hdl->ierr = 0;
	hdl->oerr = 0;
	hdl->offset = 0;
	hdl->idelta = 0;
	hdl->odelta = 0;
	hdl->infds = 0;
	hdl->onfds = 0;

	/* prepare */
	if (hdl->sio.mode & SIO_PLAY) {
		if (snd_pcm_prepare(hdl->out_pcm) < 0) {
			DPERROR("alsa_start: prepare playback failed");
			hdl->sio.eof = 1;
			return 0;
		}
		hdl->onfds = snd_pcm_poll_descriptors_count(hdl->out_pcm);
	}
	if (hdl->sio.mode & SIO_REC) {
		if (snd_pcm_prepare(hdl->in_pcm) < 0) {
			DPERROR("alsa_start: prepare record failed");
			hdl->sio.eof = 1;
			return 0;
		}
		hdl->infds = snd_pcm_poll_descriptors_count(hdl->in_pcm);
	}

	hdl->filling = 0;
	if (hdl->sio.mode & SIO_PLAY) {
		hdl->filling = 1;
	}
	if (hdl->sio.mode & SIO_REC) {
		if (snd_pcm_start(hdl->in_pcm) < 0) {
			DPERROR("alsa_start: start record failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}

	return 1;
}

static int
alsa_stop(struct sio_hdl *sh)
{
	struct alsa_hdl *hdl = (struct alsa_hdl *)sh;

	if (hdl->sio.mode & SIO_PLAY) {
		if (snd_pcm_drop(hdl->out_pcm) < 0) {
			DPERROR("alsa_stop: drop/close playback failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if (hdl->sio.mode & SIO_REC) {
		if (snd_pcm_drop(hdl->in_pcm) < 0) {
			DPERROR("alsa_stop: drop/close record failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}

	return 1;
}

static int
alsa_setpar(struct sio_hdl *sh, struct sio_par *par)
{
	struct alsa_hdl *hdl = (struct alsa_hdl *)sh;
	unsigned bufsz, round;
	unsigned irate, orate, req_rate;
	unsigned ich, och;
	snd_pcm_format_t ifmt, ofmt;
	snd_pcm_uframes_t infr, onfr;
	int fmt_err, nround;
	unsigned inp, onp;
	struct sio_enc enc;

	/* bits, bps, sig, le, msb */

	enc.bits = par->bits;
	enc.bps = par->bps;
	enc.sig = par->sig;
	enc.le = par->le;
	enc.msb = par->msb;
	alsa_enctofmt(hdl, &ofmt, &enc);
	ifmt = ofmt;
	fmt_err = 0;
	if (hdl->sio.mode & SIO_PLAY) {
 play_again:
		/* SOUND_PCM_FORMAT_FLOAT_LE is the highest enum we can
		 * support, SND_PCM_FORMAT_S16_LE is the lowest.
		 */
		while (ofmt < SND_PCM_FORMAT_FLOAT_LE) {
			if (snd_pcm_hw_params_set_format(hdl->out_pcm,
			    hdl->out_hwp, ofmt) < 0) {
				if (!fmt_err) {
					/* skip 8-bit formats */
					ofmt = SND_PCM_FORMAT_S16_LE - 1;
				}
				fmt_err++;
				ofmt++;
			} else {
				break;
			}
		}
		if (ofmt >= SND_PCM_FORMAT_FLOAT_LE) {
			DPERROR("could not set linear play format");
			hdl->sio.eof = 1;
			return 0;
		}
		if (snd_pcm_hw_params_get_format(hdl->out_hwp, &ofmt) < 0) {
			DPERROR("get play format failed");
			hdl->sio.eof = 1;
			return 0;
		}
		if (hdl->sio.mode & SIO_REC) {
			ifmt = ofmt;
			if (snd_pcm_hw_params_set_format(hdl->in_pcm,
			    hdl->in_hwp, ifmt) < 0) {
				ifmt++;
				ofmt = ifmt;
				goto play_again;
			}
		}
	} else {
		while (ifmt < SND_PCM_FORMAT_FLOAT_LE) {
			if (snd_pcm_hw_params_set_format(hdl->in_pcm,
			    hdl->in_hwp, ifmt) < 0) {
				if (!fmt_err) {
					/* skip 8-bit formats */
					ifmt = SND_PCM_FORMAT_S16_LE - 1;
				}
				fmt_err++;
				ifmt++;
			} else {
				break;
			}
		}
		if (ifmt >= SND_PCM_FORMAT_FLOAT_LE) {
			DPERROR("could not set linear record format");
			hdl->sio.eof = 1;
			return 0;
		}
		if (snd_pcm_hw_params_get_format(hdl->in_hwp, &ifmt) < 0) {
			DPERROR("get record format failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}
	alsa_fmttopar(hdl, (hdl->sio.mode & SIO_PLAY) ? ofmt : ifmt, par);

	/* rate */

	/* set a rate so we can get 1 rate back, otherwise _get_rate() fails */
	if (!par->rate || par->rate == ~0U)
		par->rate = 44100;
	req_rate = orate = irate = par->rate;
	if (hdl->sio.mode & SIO_PLAY) {
		if (snd_pcm_hw_params_set_rate_near(hdl->out_pcm, hdl->out_hwp,
		    &orate, NULL) < 0) {
			DPERROR("set play rate failed");
			hdl->sio.eof = 1;
			return 0;
		}
		if (snd_pcm_hw_params_get_rate(hdl->out_hwp, &orate, 0) < 0) {
			DPERROR("get play rate failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if (hdl->sio.mode & SIO_REC) {
		if (snd_pcm_hw_params_set_rate_near(hdl->in_pcm, hdl->in_hwp,
		    &irate, NULL) < 0) {
			DPERROR("set rec rate failed");
			hdl->sio.eof = 1;
			return 0;
		}
		if (snd_pcm_hw_params_get_rate(hdl->in_hwp, &irate, 0) < 0) {
			DPERROR("get record rate failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if ((hdl->sio.mode == (SIO_PLAY|SIO_REC)) && (orate != irate)) {
		DPERROR("could not get matching play/record rate");
		hdl->sio.eof = 1;
		return 0;
	}
	par->rate = (hdl->sio.mode & SIO_PLAY) ? orate : irate;

	/* pchan, rchan */

	och = par->pchan;
	ich = par->rchan;
	if (hdl->sio.mode & SIO_PLAY) {
		if (!och || och == ~0U)
			och = 2;
		if (snd_pcm_hw_params_set_channels_near(hdl->out_pcm,
		    hdl->out_hwp, &och) < 0) {
			DPERROR("set play channel count failed");
			hdl->sio.eof = 1;
			return 0;
		}
		if (snd_pcm_hw_params_get_channels(hdl->out_hwp, &och) < 0) {
			DPERROR("get play channel count failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if (hdl->sio.mode & SIO_REC) {
		if (!ich || ich == ~0U)
			ich = 2;
		if (snd_pcm_hw_params_set_channels_near(hdl->in_pcm,
		    hdl->in_hwp, &ich) < 0) {
			DPERROR("set record channel count failed");
			hdl->sio.eof = 1;
			return 0;
		}
		if (snd_pcm_hw_params_get_channels(hdl->in_hwp, &ich) < 0) {
			DPERROR("get record channel count failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}
	par->pchan = och;
	par->rchan = ich;

	/* round */

	/*
	 * if block size and buffer size are not both set then
	 * set the blocksize to 1/4 the buffer size
	 */
	/*
	 * If the rate that the hardware is using is different than
	 * the requested rate, scale buffer sizes so they will be the
	 * same time duration as what was requested.
	 */
	bufsz = par->appbufsz;
	round = par->round;
	if (bufsz && bufsz != ~0U) {
		bufsz = bufsz * par->rate / req_rate;
		if (round == ~0U)
			round = (bufsz + 1) / 4;
		else
			round = round * par->rate / req_rate;
	} else if (round && round != ~0U) {
		round = round * par->rate / req_rate;
		bufsz = round * 4;
	} else {
		round = par->rate / 20;
		bufsz = round * 4;
	}

	infr = onfr = round;
	if (hdl->sio.mode & SIO_PLAY) {
		if (snd_pcm_hw_params_set_period_size_near(hdl->out_pcm,
		    hdl->out_hwp, &onfr, NULL) < 0) {
			DPERROR("set play period size failed");
			hdl->sio.eof = 1;
			return 0;
		}
		if (snd_pcm_hw_params_get_period_size(hdl->out_hwp,
		    &onfr, NULL) < 0) {
			DPERROR("get play period size failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if (hdl->sio.mode == (SIO_PLAY|SIO_REC))
		infr = onfr;
	if (hdl->sio.mode & SIO_REC) {
		if (snd_pcm_hw_params_set_period_size_near(hdl->in_pcm,
		    hdl->in_hwp, &infr, NULL) < 0) {
			DPERROR("set record period size failed");
			hdl->sio.eof = 1;
			return 0;
		}
		if (snd_pcm_hw_params_get_period_size(hdl->in_hwp,
		    &infr, NULL) < 0) {
			DPERROR("get record period size failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if ((hdl->sio.mode == (SIO_PLAY|SIO_REC)) && (infr != onfr)) {
		DPERROR("could not get matching play/record period size");
		hdl->sio.eof = 1;
		return 0;
	}
	par->round = (hdl->sio.mode & SIO_PLAY) ? onfr : infr;

	/* appbufsz */

	nround = (bufsz + round - 1) / round;
	inp = onp = nround;
	if (hdl->sio.mode & SIO_PLAY) {
		snd_pcm_hw_params_set_periods_min(hdl->out_pcm, hdl->out_hwp,
		    &onp, NULL);
		/* if above returned smaller than wanted, bump it back up.
		 * if larger was returned, it's the min and we have to use it.
		 */
		if (onp < nround)
			onp = nround;
		if (snd_pcm_hw_params_set_periods_near(hdl->out_pcm,
		    hdl->out_hwp, &onp, NULL) < 0) {
			DPERROR("set play periods failed");
			hdl->sio.eof = 1;
			return 0;
		}
		if (snd_pcm_hw_params_get_periods(hdl->out_hwp,
		    &onp, NULL) < 0) {
			DPERROR("get play periods failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if (hdl->sio.mode == (SIO_PLAY|SIO_REC))
		inp = onp;
	if (hdl->sio.mode & SIO_REC) {
		snd_pcm_hw_params_set_periods_min(hdl->in_pcm, hdl->in_hwp,
		    &inp, NULL);
		/* if above returned smaller than wanted, bump it back up.
		 * if larger was returned, it's the min and we have to use it.
		 */
		if (inp < nround)
			inp = nround;
		if (snd_pcm_hw_params_set_periods_near(hdl->in_pcm, hdl->in_hwp,
		    &inp, NULL) < 0) {
			DPERROR("set record periods failed");
			hdl->sio.eof = 1;
			return 0;
		}
		if (snd_pcm_hw_params_get_periods(hdl->in_hwp,
		    &inp, NULL) < 0) {
			DPERROR("get record periods failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if ((hdl->sio.mode == (SIO_PLAY|SIO_REC)) && (inp != onp)) {
		DPERROR("could not get matching play/record period count");
		hdl->sio.eof = 1;
		return 0;
	}
	if (hdl->sio.mode & SIO_PLAY) {
		if (snd_pcm_hw_params_set_buffer_size(hdl->out_pcm,
		    hdl->out_hwp, onp * onfr) < 0) {
			DPERROR("set play buffer size failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if (hdl->sio.mode & SIO_REC) {
		if (snd_pcm_hw_params_set_buffer_size(hdl->in_pcm,
		    hdl->in_hwp, inp * infr) < 0) {
			DPERROR("set record buffer size failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}
	par->appbufsz = (hdl->sio.mode & SIO_PLAY) ? onfr * onp : infr * inp;
	par->bufsz = par->appbufsz;

	DPRINTF("alsa_setpar: pchan=%d rchan=%d rate=%d round=%d bufsz=%d\n",
	    par->pchan, par->rchan, par->rate, par->round, par->appbufsz);

	/* commit hardware params */

	if (hdl->sio.mode & SIO_PLAY) {
		if (snd_pcm_hw_params(hdl->out_pcm, hdl->out_hwp) < 0) {
			DPERROR("commit play params failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if (hdl->sio.mode & SIO_REC) {
		if (snd_pcm_hw_params(hdl->in_pcm, hdl->in_hwp) < 0) {
			DPERROR("commit record params failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}

	/* software params */

	if (hdl->sio.mode & SIO_PLAY) {
		snd_pcm_sw_params_current(hdl->out_pcm, hdl->out_swp);
		if (snd_pcm_sw_params_set_start_threshold(hdl->out_pcm,
		    hdl->out_swp, UINT_MAX) < 0) {
			DPERROR("set play start threshold failed");
			hdl->sio.eof = 1;
			return 0;
		}
		if (snd_pcm_sw_params_set_stop_threshold(hdl->out_pcm,
		    hdl->out_swp, onfr * onp) < 0) {
			DPERROR("set play stop threshold failed");
			hdl->sio.eof = 1;
			return 0;
		}
		if (snd_pcm_sw_params_set_avail_min(hdl->out_pcm,
		    hdl->out_swp, onfr) < 0) {
			DPERROR("set play min avail failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}

	if (hdl->sio.mode & SIO_REC) {
		snd_pcm_sw_params_current(hdl->in_pcm, hdl->in_swp);
		if (snd_pcm_sw_params_set_start_threshold(hdl->in_pcm,
		    hdl->in_swp, UINT_MAX) < 0) {
			DPERROR("set record start threshold failed");
			hdl->sio.eof = 1;
			return 0;
		}
		if (snd_pcm_sw_params_set_stop_threshold(hdl->in_pcm,
		    hdl->in_swp, infr * inp) < 0) {
			DPERROR("set record stop threshold failed");
			hdl->sio.eof = 1;
			return 0;
		}
		if (snd_pcm_sw_params_set_avail_min(hdl->in_pcm,
		    hdl->in_swp, infr) < 0) {
			DPERROR("set record min avail failed");
			hdl->sio.eof = 1;
			return 0;
		}
	}

	return 1;
}

static int
alsa_getpar(struct sio_hdl *sh, struct sio_par *par)
{
	struct alsa_hdl *hdl = (struct alsa_hdl *)sh;
	snd_pcm_format_t fmt;
	snd_pcm_uframes_t nfr;
	uint nch, rate;
	int dir;

	/* bit, sig, le, msb */

	if (hdl->sio.mode & SIO_PLAY) {
		if (snd_pcm_hw_params_get_format(hdl->out_hwp, &fmt) < 0) {
			DPERROR("alsa_getpar: get play hw format");
			hdl->sio.eof = 1;
			return 0;
		}
	} else if (hdl->sio.mode & SIO_REC) {
		if (snd_pcm_hw_params_get_format(hdl->in_hwp, &fmt) < 0) {
			DPERROR("alsa_getpar: get rec hw format");
			hdl->sio.eof = 1;
			return 0;
		}
	}
	alsa_fmttopar(hdl, fmt, par);

	/* pchan, rchan */

	if (hdl->sio.mode & SIO_PLAY) {
		if (snd_pcm_hw_params_get_channels(hdl->out_hwp, &nch) < 0) {
			DPERROR("alsa_getpar: get play hw channels");
			hdl->sio.eof = 1;
			return 0;
		}
		par->pchan = nch;
	}
	if (hdl->sio.mode & SIO_REC) {
		if (snd_pcm_hw_params_get_channels(hdl->in_hwp, &nch) < 0) {
			DPERROR("alsa_getpar: get rec hw channels");
			hdl->sio.eof = 1;
			return 0;
		}
		par->rchan = nch;
	}

	/* rate */

	if (hdl->sio.mode & SIO_PLAY) {
		if (snd_pcm_hw_params_get_rate(hdl->out_hwp, &rate, &dir) < 0) {
			DPERROR("alsa_getpar: get play hw rate");
			hdl->sio.eof = 1;
			return 0;
		}
	} else if (hdl->sio.mode & SIO_REC) {
		if (snd_pcm_hw_params_get_rate(hdl->in_hwp, &rate, &dir) < 0) {
			DPERROR("alsa_getpar: get rec hw rate");
			hdl->sio.eof = 1;
			return 0;
		}
	}
	par->rate = rate;

	/* round */

	if (hdl->sio.mode & SIO_PLAY) {
		if (snd_pcm_hw_params_get_period_size(hdl->out_hwp,
		    &nfr, &dir) < 0) {
			DPERROR("alsa_getpar: get play hw round");
			hdl->sio.eof = 1;
			return 0;
		}
	} else if (hdl->sio.mode & SIO_REC) {
		if (snd_pcm_hw_params_get_period_size(hdl->in_hwp,
		    &nfr, &dir) < 0) {
			DPERROR("alsa_getpar: get rec hw round");
			hdl->sio.eof = 1;
			return 0;
		}
	}
	par->round = nfr;

	/* appbufsz */

	if (hdl->sio.mode & SIO_PLAY) {
		if (snd_pcm_hw_params_get_buffer_size(hdl->out_hwp, &nfr) < 0) {
			DPERROR("alsa_getpar: get play hw bufsz");
			hdl->sio.eof = 1;
			return 0;
		}
	} else if (hdl->sio.mode & SIO_REC) {
		if (snd_pcm_hw_params_get_buffer_size(hdl->in_hwp, &nfr) < 0) {
			DPERROR("alsa_getpar: get rec hw bufsz");
			hdl->sio.eof = 1;
			return 0;
		}
	}
	par->appbufsz = nfr;
	par->bufsz = par->appbufsz;

	return 1;
}

/*
 * drop recorded samples to compensate xruns
 */
static int
alsa_rdrop(struct alsa_hdl *hdl)
{
#define DROP_NMAX 0x1000
	static char dropbuf[DROP_NMAX];
	snd_pcm_sframes_t n;
	snd_pcm_uframes_t todo;
	int drop_nmax = DROP_NMAX / hdl->ibpf;

	while (hdl->offset > 0) {
		todo = hdl->offset;
		if (todo > drop_nmax)
			todo = drop_nmax;
		while ((n = snd_pcm_readi(hdl->in_pcm, dropbuf, todo)) < 0) {
			if (n == -ESTRPIPE)
				continue;
			DPERROR("alsa_rdrop: readi");
			hdl->sio.eof = 1;
			return 0;
		}
		if (n == 0) {
			DPRINTF("alsa_rdrop: eof\n");
			hdl->sio.eof = 1;
			return 0;
		}
		hdl->offset -= (int)n;
		//hdl->isfr += (int)n;
		DPRINTF("alsa_rdrop: dropped %ld/%ld frames\n", n, todo);
	}
	return 1;
}

static size_t
alsa_read(struct sio_hdl *sh, void *buf, size_t len)
{
	struct alsa_hdl *hdl = (struct alsa_hdl *)sh;
	snd_pcm_sframes_t n;
	snd_pcm_uframes_t todo;

	if (!alsa_rdrop(hdl))
		return 0;
	todo = len / hdl->ibpf;
	while ((n = snd_pcm_readi(hdl->in_pcm, buf, todo)) < 0) {
		if (n == -ESTRPIPE)
			continue;
		DPERROR("alsa_read: read");
		hdl->sio.eof = 1;
		return 0;
	}
	if (n == 0) {
		DPRINTF("alsa_read: eof\n");
		hdl->sio.eof = 1;
		return 0;
	}
	hdl->isfr += n;
	n *= hdl->ibpf;
	return n;
}

static size_t
alsa_autostart(struct alsa_hdl *hdl)
{
	struct pollfd *pfd;
	int ret, state;
	unsigned short revents;

	pfd = malloc(sizeof(struct pollfd) * hdl->onfds);
	if (pfd == NULL) {
		DPERROR("alsa_autostart: allocate pfd");
		hdl->sio.eof = 1;
		return 0;
	}
	ret = snd_pcm_poll_descriptors(hdl->out_pcm, pfd, hdl->onfds);
	if (ret != hdl->onfds) {
		DPERROR("alsa_autostart: snd_pcm_poll_descriptors");
		hdl->sio.eof = 1;
		goto bad_free;
	}
	pfd->events = POLLOUT;
	while (poll(pfd, hdl->onfds, 0) < 0) {
		if (errno == EINTR)
			continue;
		DPERROR("alsa_autostart: poll");
		hdl->sio.eof = 1;
		goto bad_free;
	}
	if (snd_pcm_poll_descriptors_revents(hdl->out_pcm, pfd, hdl->onfds,
	    &revents) < 0) {
		DPERROR("alsa_autostart: snd_pcm_poll_descriptors");
		goto bad_free;
	}
	if (!(revents & POLLOUT)) {
		hdl->filling = 0;
		if (hdl->sio.mode & SIO_PLAY) {
			state = snd_pcm_state(hdl->out_pcm);
			if (state == SND_PCM_STATE_PREPARED) {
				if (snd_pcm_start(hdl->out_pcm) < 0) {
					DPERROR("alsa_autostart: play failed");
					goto bad_free;
				}
			} else if (state != SND_PCM_STATE_RUNNING) {
				DPERROR("alsa_autostart: bad play state");
				goto bad_free;
			}
		}
		sio_onmove_cb(&hdl->sio, 0);
	}
	free(pfd);
	return 1;

 bad_free:
	free(pfd);
	return 0;
}

/*
 * insert silence to play to compensate xruns
 */
static int
alsa_wsil(struct alsa_hdl *hdl)
{
#define ZERO_NMAX 0x1000
	static char zero[ZERO_NMAX];
	snd_pcm_uframes_t n;
	snd_pcm_sframes_t todo;
	int zero_nmax = ZERO_NMAX / hdl->obpf;

	while (hdl->offset < 0) {
		todo = (int)-hdl->offset;
		if (todo > zero_nmax)
			todo = zero_nmax;
		if ((n = snd_pcm_writei(hdl->out_pcm, zero, todo)) < 0) {
			if (errno == -EBADFD)
				DPERROR("alsa_wsil: PCM not in the right state");
			else if (errno == -EPIPE)
				DPERROR("alsa_wsil: underrun");
			else if (errno == -ESTRPIPE)
				DPERROR("alsa_wsil: stream is suspended");
			hdl->sio.eof = 1;
			return 0;
		}
		hdl->offset += (int)n;
		//hdl->osfr += (int)n;
		DPRINTF("alsa_wsil: inserted %ld/%ld frames\n", n, todo);
	}
	return 1;
}

static size_t
alsa_write(struct sio_hdl *sh, const void *buf, size_t len)
{
	struct alsa_hdl *hdl = (struct alsa_hdl *)sh;
	const unsigned char *data = buf;
	ssize_t n, todo;

	if (!alsa_wsil(hdl))
		return 0;
	todo = len / hdl->obpf;
	if ((n = snd_pcm_writei(hdl->out_pcm, data, todo)) < 0) {
		if (errno == -EBADFD)
			DPERROR("alsa_write: PCM not in the right state");
		else if (errno == -EPIPE)
			DPERROR("alsa_write: underrun");
		else if (errno == -ESTRPIPE)
			DPERROR("alsa_write: stream is suspended");
 		return 0;
	}
	if (hdl->filling) {
		if (!alsa_autostart(hdl))
			return 0;
	}
	hdl->osfr += n;
	n *= hdl->obpf;
	return n;
}

static int
alsa_pollfd(struct sio_hdl *sh, struct pollfd *pfd, int events)
{
	struct alsa_hdl *hdl = (struct alsa_hdl *)sh;
	int nfds, ret, i;

	nfds = 0;
	if (hdl->sio.mode & SIO_PLAY) {
		ret = snd_pcm_poll_descriptors(hdl->out_pcm, &pfd[0],
		    hdl->onfds);
		if (ret != hdl->onfds) {
			DPERROR("alsa_pollfd: play snd_pcm_poll_descriptors");
			hdl->sio.eof = 1;
			return 0;
		}
		nfds += ret;
	}
	if (hdl->sio.mode & SIO_REC) {
		ret = snd_pcm_poll_descriptors(hdl->in_pcm, &pfd[nfds],
		    hdl->infds);
		if (ret != hdl->infds) {
			DPERROR("alsa_pollfd: record snd_pcm_poll_descriptors");
			hdl->sio.eof = 1;
			return 0;
		}
		nfds += ret;
	}

	for (i = 0; i < nfds; i++) {
		/* user events */
		pfd[i].events |= events;

		/* ALSA doesn't set POLLERR in some versions ? */
		pfd[i].events |= POLLERR;
	}

	return nfds;
}

int
alsa_revents(struct sio_hdl *sh, struct pollfd *pfd)
{
	struct alsa_hdl *hdl = (struct alsa_hdl *)sh;
	snd_pcm_sframes_t idelay, odelay;
	snd_pcm_state_t istate, ostate;
	int hw_ptr, nfds;
	unsigned short revents, all_revents;

	all_revents = nfds = 0;
	if (hdl->sio.mode & SIO_PLAY) {
		ostate = snd_pcm_state(hdl->out_pcm);
		if (ostate == SND_PCM_STATE_XRUN) {
			printf("alsa_revents: play xrun\n");
		}
		if (snd_pcm_poll_descriptors_revents(hdl->out_pcm, &pfd[0],
		    hdl->onfds, &revents) < 0) {
			DPERROR("alsa_revents: snd_pcm_poll_descriptors");
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		if (revents & POLLERR)
			DPERROR("alsa_revents: play xrun?");
		all_revents |= revents;
		nfds += hdl->onfds;
	}
	if (hdl->sio.mode & SIO_REC) {
		istate = snd_pcm_state(hdl->in_pcm);
		if (istate == SND_PCM_STATE_XRUN) {
			printf("alsa_revents: record xrun\n");
		}
		if (snd_pcm_poll_descriptors_revents(hdl->in_pcm, &pfd[nfds],
		    hdl->infds, &revents) < 0) {
			DPERROR("alsa_revents: snd_pcm_poll_descriptors");
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		if (revents & POLLERR)
			DPERROR("alsa_revents: record xrun?");
		all_revents |= revents;
		nfds += hdl->infds;
	}
	revents = all_revents;

	if ((revents & POLLOUT) && (hdl->sio.mode & SIO_PLAY) &&
	    (ostate == SND_PCM_STATE_RUNNING ||
	     ostate == SND_PCM_STATE_PREPARED)) {
		if (snd_pcm_avail_update(hdl->out_pcm) < 0) {
			DPERROR("alsa_revents: play snd_pcm_avail_update");
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		if (snd_pcm_delay(hdl->out_pcm, &odelay) < 0) {
			DPERROR("alsa_revents: play snd_pcm_delay");
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		if (odelay < 0) {
			printf("alsa_revents: play xrun (delay)\n");
		}
		hw_ptr = hdl->osfr - odelay;
		hdl->odelta += hw_ptr - hdl->ohfr;
		hdl->ohfr = hw_ptr;
		if (hdl->odelta > 0) {
			sio_onmove_cb(&hdl->sio, hdl->odelta);
			hdl->odelta = 0;
		}
	}
	if ((revents & POLLIN) && !(hdl->sio.mode & SIO_PLAY) &&
	    (istate == SND_PCM_STATE_RUNNING ||
	     istate == SND_PCM_STATE_PREPARED)) {
		if (snd_pcm_avail_update(hdl->in_pcm) < 0) {
			DPERROR("alsa_revents: record snd_pcm_avail_update");
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		if (snd_pcm_delay(hdl->in_pcm, &idelay) < 0) {
			DPERROR("alsa_revents: record snd_pcm_delay");
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		if (idelay < 0) {
			printf("alsa_revents: record xrun (delay)\n");
		}
		hw_ptr = hdl->isfr + idelay;
		hdl->idelta += hw_ptr - hdl->ihfr;
		hdl->ihfr = hw_ptr;
		if (hdl->idelta > 0) {
			sio_onmove_cb(&hdl->sio, hdl->idelta);
			hdl->idelta = 0;
		}
	}

	/*
	 * drop recorded samples or insert silence to play
	 * right now to adjust revents, and avoid busy loops
	 * programs
	 */
	if (hdl->sio.started) {
		if (hdl->filling)
			revents |= POLLOUT;
		if ((hdl->sio.mode & SIO_PLAY) && !alsa_wsil(hdl))
			revents &= ~POLLOUT;
		if ((hdl->sio.mode & SIO_REC) && !alsa_rdrop(hdl))
			revents &= ~POLLIN;
	}
	return revents;
}
#endif /* defined USE_ALSA */
