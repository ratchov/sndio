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

#include "sio_priv.h"

#ifdef DEBUG
#define DALSA(str, err) fprintf(stderr, "%s: %s\n", str, snd_strerror(err)) 
#else
#define DALSA(str, err) do {} while (0)
#endif

struct sio_alsa_hdl {
	struct sio_hdl sio;
	struct sio_par par;
	struct pollfd *pfds;
	snd_pcm_t *out_pcm;
	snd_pcm_t *in_pcm;
	snd_pcm_hw_params_t *out_hwp;
	snd_pcm_sw_params_t *out_swp;
	snd_pcm_hw_params_t *in_hwp;
	snd_pcm_sw_params_t *in_swp;
	int filling, filltodo;
	unsigned ibpf, obpf;		/* bytes per frame */
	unsigned ihfr, ohfr;		/* frames the hw transfered */
	unsigned isfr, osfr;		/* frames the sw transfered */
	unsigned ierr, oerr;		/* frames the hw dropped */
	int offset;			/* frames play is ahead of record */
	int idelta, odelta;		/* position reported to client */
	int nfds, infds, onfds;
};

static void sio_alsa_close(struct sio_hdl *);
static int sio_alsa_start(struct sio_hdl *);
static int sio_alsa_stop(struct sio_hdl *);
static int sio_alsa_setpar(struct sio_hdl *, struct sio_par *);
static int sio_alsa_getpar(struct sio_hdl *, struct sio_par *);
static int sio_alsa_getcap(struct sio_hdl *, struct sio_cap *);
static size_t sio_alsa_read(struct sio_hdl *, void *, size_t);
static size_t sio_alsa_write(struct sio_hdl *, const void *, size_t);
static int sio_alsa_nfds(struct sio_hdl *);
static int sio_alsa_pollfd(struct sio_hdl *, struct pollfd *, int);
static int sio_alsa_revents(struct sio_hdl *, struct pollfd *);
static int sio_alsa_setvol(struct sio_hdl *, unsigned);
static void sio_alsa_getvol(struct sio_hdl *);

static struct sio_ops sio_alsa_ops = {
	sio_alsa_close,
	sio_alsa_setpar,
	sio_alsa_getpar,
	sio_alsa_getcap,
	sio_alsa_write,
	sio_alsa_read,
	sio_alsa_start,
	sio_alsa_stop,
	sio_alsa_nfds,
	sio_alsa_pollfd,
	sio_alsa_revents,
	sio_alsa_setvol,
	sio_alsa_getvol
};

/*
 * convert ALSA format to sio_par encoding
 */
static int
sio_alsa_fmttopar(struct sio_alsa_hdl *hdl, snd_pcm_format_t fmt, struct sio_par *par)
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
		DPRINTF("sio_alsa_fmttopar: 0x%x: unsupported format\n", fmt);
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
sio_alsa_enctofmt(struct sio_alsa_hdl *hdl, snd_pcm_format_t *rfmt, struct sio_par *enc)
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
sio_alsa_tryinfo(struct sio_alsa_hdl *hdl, struct sio_enc *enc,
    unsigned pchan, unsigned rchan, unsigned rate)
{
	snd_pcm_format_t fmt;

	sio_alsa_enctofmt(hdl, &fmt, enc);
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
sio_alsa_getcap(struct sio_hdl *sh, struct sio_cap *cap)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;

	DPRINTF("sio_alsa_getcap: not implemented\n");
	hdl->sio.eof = 1;
	return 0;
}

static void
sio_alsa_getvol(struct sio_hdl *sh)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;

	sio_onvol_cb(&hdl->sio, SIO_MAXVOL);
}

int
sio_alsa_setvol(struct sio_hdl *sh, unsigned vol)
{
	return 1;
}

struct sio_hdl *
sio_open_alsa(const char *str, unsigned mode, int nbio)
{
	struct sio_alsa_hdl *hdl;
	char path[PATH_MAX];
	struct sio_par par;
	int err;

	hdl = malloc(sizeof(struct sio_alsa_hdl));
	if (hdl == NULL)
		return NULL;
	sio_create(&hdl->sio, &sio_alsa_ops, mode, nbio);

	snprintf(path, sizeof(path), "hw:%s", str);

	if (mode & SIO_PLAY) {
		err = snd_pcm_open(&hdl->out_pcm, path,
		    SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
		if (err < 0) {
			DALSA("snd_pcm_open", err);
			goto bad_free;
		}
	}
	if (mode & SIO_REC) {
		err = snd_pcm_open(&hdl->in_pcm, path,
		    SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
		if (err < 0) {
			DALSA("snd_pcm_open", err);
			goto bad_free_out_pcm;
		}
	}
	if (mode & SIO_PLAY) {
		err = snd_pcm_hw_params_malloc(&hdl->out_hwp);
		if (err < 0) {
			DALSA("could not alloc out_hwp", err);
			goto bad_free_in_pcm;
		}
		err = snd_pcm_sw_params_malloc(&hdl->out_swp);
		if (err < 0) {
			DALSA("could not alloc out_swp", err);
			goto bad_free_out_hwp;
		}
	}
	if (mode & SIO_REC) {
		err = snd_pcm_hw_params_malloc(&hdl->in_hwp);
		if (err < 0) {
			DALSA("could not alloc in_hwp", err);
			goto bad_free_out_swp;
		}
		err = snd_pcm_sw_params_malloc(&hdl->in_swp);
		if (err < 0) {
			DALSA("could not alloc in_swp", err);
			goto bad_free_in_hwp;
		}
	}
	if ((mode & SIO_PLAY) && (mode & SIO_REC)) {
		err = snd_pcm_link(hdl->in_pcm, hdl->out_pcm);
		if (err < 0) {
			DALSA("could not alloc in_swp", err);
			goto bad_free_in_swp;
		}
	}
	hdl->nfds = 0;
	if (mode & SIO_PLAY)
		hdl->nfds += snd_pcm_poll_descriptors_count(hdl->out_pcm);
	if (mode & SIO_REC)
		hdl->nfds += snd_pcm_poll_descriptors_count(hdl->in_pcm);
	hdl->pfds = malloc(sizeof(struct pollfd) * hdl->nfds);
	if (hdl->pfds == NULL) {
		DPERROR("couldn't allocate pollfd structures");
		goto bad_free_in_swp;
	}
	DPRINTF("allocated %d descriptors\n", hdl->nfds);

	/*
	 * Default parameters may not be compatible with libsndio (eg. mulaw
	 * encodings, different playback and recording parameters, etc...), so
	 * set parameters to a random value. If the requested parameters are
	 * not supported by the device, then sio_setpar() will pick supported
	 * ones.
	 */
	sio_initpar(&par);
	par.bits = 16;
	par.le = SIO_LE_NATIVE;
	par.rate = 48000;
	if (mode & SIO_PLAY)
		par.pchan = 2;
	if (mode & SIO_REC)
		par.rchan = 2;
	if (!sio_setpar(&hdl->sio, &par))
		goto bad_free_in_swp;
	return (struct sio_hdl *)hdl;
bad_free_in_swp:
 	if (mode & SIO_REC)
		snd_pcm_sw_params_free(hdl->in_swp);
bad_free_in_hwp:
 	if (mode & SIO_REC)
		snd_pcm_hw_params_free(hdl->in_hwp);
bad_free_out_swp:
 	if (mode & SIO_PLAY)
		snd_pcm_sw_params_free(hdl->out_swp);
bad_free_out_hwp:
 	if (mode & SIO_PLAY)
		snd_pcm_hw_params_free(hdl->out_hwp);
bad_free_in_pcm:
	if (mode & SIO_REC)
		snd_pcm_close(hdl->in_pcm);
bad_free_out_pcm:
	if (mode & SIO_PLAY)
		snd_pcm_close(hdl->out_pcm);
bad_free:
	free(hdl);
	return NULL;
}

static void
sio_alsa_close(struct sio_hdl *sh)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;

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
sio_alsa_start(struct sio_hdl *sh)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;
	int err;

	DPRINTF("sio_alsa_start:\n");

	hdl->ibpf = hdl->par.rchan * hdl->par.bps;
	hdl->obpf = hdl->par.pchan * hdl->par.bps;
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

	if (hdl->sio.mode & SIO_PLAY) {
		err = snd_pcm_prepare(hdl->out_pcm);
		if (err < 0) {
			DALSA("sio_alsa_start: prepare playback failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_prepare(hdl->in_pcm);
		if (err < 0) {
			DALSA("sio_alsa_start: prepare record failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	hdl->filling = 0;
	if (hdl->sio.mode & SIO_PLAY) {
		hdl->filling = 1;
		hdl->filltodo = hdl->par.bufsz;
	} else {
		err = snd_pcm_start(hdl->in_pcm);
		if (err < 0) {
			DALSA("sio_alsa_start: start record failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	return 1;
}

static int
sio_alsa_stop(struct sio_hdl *sh)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;
	int err;

	if (hdl->sio.mode & SIO_PLAY) {
		err = snd_pcm_drop(hdl->out_pcm);
		if (err < 0) {
			DALSA("sio_alsa_stop: drop/close playback failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_drop(hdl->in_pcm);
		if (err < 0) {
			DALSA("sio_alsa_stop: drop/close record failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}

	return 1;
}

static int
sio_alsa_setpar(struct sio_hdl *sh, struct sio_par *par)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;
	unsigned bufsz, round;
	unsigned irate, orate, req_rate;
	unsigned ich, och;
	snd_pcm_format_t ifmt, ofmt;
	snd_pcm_uframes_t infr, onfr;
	snd_pcm_uframes_t ibufsz, obufsz;
	int err;

	/*
	 * set encoding
	 */
	sio_alsa_enctofmt(hdl, &ofmt, par);
	DPRINTF("ofmt = %u\n", ofmt);
	if (hdl->sio.mode & SIO_PLAY) {
		err = snd_pcm_hw_params_any(hdl->out_pcm, hdl->out_hwp);
		if (err < 0) {
			DALSA("couldn't init play pars", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_hw_params_set_access(hdl->out_pcm,
		    hdl->out_hwp, SND_PCM_ACCESS_RW_INTERLEAVED);
		if (err < 0) {
			DALSA("couldn't set play access", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_hw_params_set_format(hdl->out_pcm,
		    hdl->out_hwp, ofmt);
		if (err < 0) {
			DALSA("couldn't set play fmt", err);
			hdl->sio.eof = 1;
			/*
			 * XXX: try snd_pcm_set_format_mask
			 */
			return 0;
		}
		err = snd_pcm_hw_params_get_format(hdl->out_hwp, &ofmt);
		if (err < 0) {
			DALSA("couldn't get play fmt", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	ifmt = ofmt;
	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_hw_params_any(hdl->in_pcm, hdl->in_hwp);
		if (err < 0) {
			DALSA("couldn't init rec pars", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_hw_params_set_access(hdl->out_pcm,
		    hdl->out_hwp, SND_PCM_ACCESS_RW_INTERLEAVED);
		if (err < 0) {
			DALSA("couldn't set rec access", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_hw_params_set_format(hdl->in_pcm,
		    hdl->in_hwp, ifmt);
		if (err < 0) {
			DALSA("couldn't set rec fmt", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_hw_params_get_format(hdl->in_hwp, &ifmt);
		if (err < 0) {
			DALSA("couldn't get play fmt", err);
			hdl->sio.eof = 1;
			return 0;
		}
		if (!(hdl->sio.mode & SIO_PLAY))
			ofmt = ifmt;
	}
	if (ifmt != ofmt) {
		DPRINTF("play and rec formats differ\n");
		hdl->sio.eof = 1;
		return 0;
	}
	if (!sio_alsa_fmttopar(hdl, ofmt, &hdl->par))
		return 0;

	/*
	 * set rate
	 */
	orate = (par->rate == ~0U) ? 48000 : par->rate;
	if (hdl->sio.mode & SIO_PLAY) {
		err = snd_pcm_hw_params_set_rate_near(hdl->out_pcm,
		    hdl->out_hwp, &orate, 0);
		if (err < 0) {
			DALSA("couldn't set play rate", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	irate = orate;
	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_hw_params_set_rate_near(hdl->in_pcm,
		    hdl->in_hwp, &irate, 0);
		if (err < 0) {
			DALSA("couldn't set rec rate", err);
			hdl->sio.eof = 1;
			return 0;
		}
		if (!(hdl->sio.mode & SIO_PLAY))
			orate = irate;
	}
	if (irate != orate) {
		DPRINTF("could not get matching play/record rate");
		hdl->sio.eof = 1;
		return 0;
	}
	hdl->par.rate = orate;

	/*
	 * set number of channels
	 */
	if ((hdl->sio.mode & SIO_PLAY) && par->pchan != ~0U) {
		och = par->pchan;
		err = snd_pcm_hw_params_set_channels_near(hdl->out_pcm,
		    hdl->out_hwp, &och);
		if (err < 0) {
			DALSA("set play channel count failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		hdl->par.pchan = och;
	}
	if ((hdl->sio.mode & SIO_REC) && par->rchan != ~0U) {
		ich = par->rchan;
		err = snd_pcm_hw_params_set_channels_near(hdl->in_pcm,
		    hdl->in_hwp, &ich);
		if (err < 0) {
			DALSA("set record channel count failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		hdl->par.rchan = ich;
	}


	/* XXX: factor this code chunk with sun backend */

	/*
	 * If the rate that the hardware is using is different than
	 * the requested rate, scale buffer sizes so they will be the
	 * same time duration as what was requested.  This just gets
	 * the rates to use for scaling, that actual scaling is done
	 * later.
	 */
	req_rate = par->rate != ~0U ? par->rate : hdl->par.rate;
	DPRINTF("req_rate = %u, orate = %u\n", req_rate, orate);

	/*
	 * if block size and buffer size are not both set then
	 * set the blocksize to half the buffer size
	 */
	bufsz = par->appbufsz;
	round = par->round;
	if (bufsz != ~0U) {
		bufsz = bufsz * orate / req_rate;
		if (round == ~0U)
			round = (bufsz + 1) / 2;
		else
			round = round * orate / req_rate;
	} else if (round != ~0U) {
		round = round * orate / req_rate;
		bufsz = round * 2;
	} else
		return 1;

	DPRINTF("sio_alsa_setpar: trying bufsz = %u, round = %u\n", bufsz, round);

	obufsz = bufsz;
	if (hdl->sio.mode & SIO_PLAY) {
		err = snd_pcm_hw_params_set_buffer_size_near(hdl->out_pcm,
		    hdl->out_hwp, &obufsz);
		if (err < 0) {
			DALSA("set play bufsz failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		DPRINTF("sio_alsa_setpar: obufsz: ok\n");
	}
	ibufsz = obufsz;
	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_hw_params_set_buffer_size_near(hdl->in_pcm,
		    hdl->in_hwp, &ibufsz);
		if (err < 0) {
			DALSA("set record buffsz failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		if (!(hdl->sio.mode & SIO_PLAY))
			ibufsz = obufsz;
	}
	if (ibufsz != obufsz) {
		DPRINTF("could not get matching play/record buffer size");
		hdl->sio.eof = 1;
		return 0;
	}
	hdl->par.appbufsz = hdl->par.bufsz = obufsz;

	onfr = round;
	if (hdl->sio.mode & SIO_PLAY) {
		err = snd_pcm_hw_params_set_period_size_near(hdl->out_pcm,
		    hdl->out_hwp, &onfr, NULL);
		if (err < 0) {
			DALSA("set play period size failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		DPRINTF("sio_alsa_setpar: onfr: ok\n");
	}
	infr = onfr;
	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_hw_params_set_period_size_near(hdl->in_pcm,
		    hdl->in_hwp, &infr, NULL);
		if (err < 0) {
			DALSA("set record period size failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		if (!(hdl->sio.mode & SIO_PLAY))
			onfr = infr;
	}
	if (infr != onfr) {
		DPRINTF("could not get matching play/record period size");
		hdl->sio.eof = 1;
		return 0;
	}
	hdl->par.round = onfr;

	DPRINTF("sio_alsa_setpar: got bufsz = %u, round = %u\n",
	    hdl->par.bufsz, hdl->par.round);



	/* commit hardware params */

	if (hdl->sio.mode & SIO_PLAY) {
		err = snd_pcm_hw_params(hdl->out_pcm, hdl->out_hwp);
		if (err < 0) {
			DALSA("commit play params failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		DPRINTF("sio_alsa_setpar: out_hwp: ok\n");
	}
	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_hw_params(hdl->in_pcm, hdl->in_hwp);
		if (err < 0) {
			DALSA("commit record params failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}

	/* software params */

	if (hdl->sio.mode & SIO_PLAY) {
		err = snd_pcm_sw_params_current(hdl->out_pcm, hdl->out_swp);
		if (err < 0) {
			DALSA("snd_pcm_sw_params_current", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_start_threshold(hdl->out_pcm,
		    hdl->out_swp, INT_MAX);
		if (err < 0) {
			DALSA("set play start threshold failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_avail_min(hdl->out_pcm,
		    hdl->out_swp, onfr);
		if (err < 0) {
			DALSA("set play avail min failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params(hdl->out_pcm, hdl->out_swp);
		if (err < 0) {
			DALSA("commit play sw params failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}

	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_sw_params_current(hdl->in_pcm, hdl->in_swp);
		if (err < 0) {
			DALSA("snd_pcm_sw_params_current", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_start_threshold(hdl->in_pcm,
		    hdl->in_swp, INT_MAX);
		if (err < 0) {
			DALSA("set record start threshold failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_avail_min(hdl->in_pcm,
		    hdl->in_swp, infr);
		if (err < 0) {
			DALSA("set rec avail min failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params(hdl->in_pcm, hdl->in_swp);
		if (err < 0) {
			DALSA("commit record sw params failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	DPRINTF("sio_alsa_setpar: done\n");
	return 1;
}

static int
sio_alsa_getpar(struct sio_hdl *sh, struct sio_par *par)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;

	*par = hdl->par;
	return 1;
}

/*
 * drop recorded samples to compensate xruns
 */
static int
sio_alsa_rdrop(struct sio_alsa_hdl *hdl)
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
			DALSA("sio_alsa_rdrop: readi", n);
			hdl->sio.eof = 1;
			return 0;
		}
		if (n == 0) {
			DPRINTF("sio_alsa_rdrop: eof\n");
			hdl->sio.eof = 1;
			return 0;
		}
		hdl->offset -= (int)n;
		//hdl->isfr += (int)n;
		DPRINTF("sio_alsa_rdrop: dropped %ld/%ld frames\n", n, todo);
	}
	return 1;
}

static size_t
sio_alsa_read(struct sio_hdl *sh, void *buf, size_t len)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;
	snd_pcm_sframes_t n;
	snd_pcm_uframes_t todo;

	if (!sio_alsa_rdrop(hdl))
		return 0;
	todo = len / hdl->ibpf;
	while ((n = snd_pcm_readi(hdl->in_pcm, buf, todo)) < 0) {
		if (n == -ESTRPIPE)
			continue;
		DALSA("sio_alsa_read: read", n);
		hdl->sio.eof = 1;
		return 0;
	}
	if (n == 0) {
		DPRINTF("sio_alsa_read: eof\n");
		hdl->sio.eof = 1;
		return 0;
	}
	hdl->isfr += n;
	n *= hdl->ibpf;
	return n;
}

static size_t
sio_alsa_autostart(struct sio_alsa_hdl *hdl)
{
	int state, err;

	state = snd_pcm_state(hdl->out_pcm);
	if (state == SND_PCM_STATE_PREPARED) {
		err = snd_pcm_start(hdl->out_pcm);
		if (err < 0) {
			DALSA("sio_alsa_autostart: failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
	} else {
		DPRINTF("sio_alsa_autostart: bad state");
		hdl->sio.eof = 1;
		return 0;
	}
	DPRINTF("sio_alsa_autostart: started\n");
	sio_onmove_cb(&hdl->sio, 0);
	return 1;
}

/*
 * insert silence to play to compensate xruns
 */
static int
sio_alsa_wsil(struct sio_alsa_hdl *hdl)
{
#define ZERO_NMAX 0x1000
	static char zero[ZERO_NMAX];
	snd_pcm_uframes_t n;
	snd_pcm_sframes_t todo;
	int zero_nmax = ZERO_NMAX / hdl->obpf;

	while (hdl->offset < 0) {
		DPRINTF("sio_alsa_wsil:\n");

		todo = (int)-hdl->offset;
		if (todo > zero_nmax)
			todo = zero_nmax;
		if ((n = snd_pcm_writei(hdl->out_pcm, zero, todo)) < 0) {
			DALSA("sio_alsa_wsil", n);
			hdl->sio.eof = 1;
			return 0;
		}
		hdl->offset += (int)n;
		//hdl->osfr += (int)n;
		DPRINTF("sio_alsa_wsil: inserted %ld/%ld frames\n", n, todo);
	}
	return 1;
}

static size_t
sio_alsa_write(struct sio_hdl *sh, const void *buf, size_t len)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;
	ssize_t n, todo;

	if (!sio_alsa_wsil(hdl))
		return 0;
	todo = len / hdl->obpf;
	if (hdl->filling && todo > hdl->filltodo)
		todo = hdl->filltodo;
	DPRINTF("sio_alsa_write: len = %zd, todo = %zd\n", len, todo);
	while ((n = snd_pcm_writei(hdl->out_pcm, buf, todo)) < 0) {
		if (n == -EINTR)
			continue;
		if (n != -EAGAIN) {
			DALSA("sio_alsa_write", n);
			hdl->sio.eof = 1;
		}
		return 0;
	}
	DPRINTF("wrote %zd\n", n);
	if (hdl->filling) {
		hdl->filltodo -= n;
		if (hdl->filltodo == 0) {
			hdl->filling = 0;
			if (!sio_alsa_autostart(hdl))
				return 0;
		}
	}
	hdl->osfr += n;
	n *= hdl->obpf;
	return n;
}

static int
sio_alsa_nfds(struct sio_hdl *sh)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;

	return hdl->nfds;
}


static int
sio_alsa_pollfd(struct sio_hdl *sh, struct pollfd *pfd, int events)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;
	int i;

	DPRINTF("sio_alsa_pollfd: count = %d\n", 
	    snd_pcm_poll_descriptors_count(hdl->out_pcm));

	memset(pfd, 0, sizeof(struct pollfd) * hdl->nfds);
	if (hdl->sio.mode & SIO_PLAY) {
		hdl->onfds = snd_pcm_poll_descriptors(hdl->out_pcm,
		    pfd, hdl->nfds);
	} else
		hdl->onfds = 0;
	if (hdl->sio.mode & SIO_REC) {
		hdl->infds = snd_pcm_poll_descriptors(hdl->in_pcm,
		    pfd + hdl->onfds, hdl->nfds - hdl->onfds);
	}
	DPRINTF("sio_alsa_pollfd: events = %x, nfds = %d + %d\n",
	    events, hdl->onfds, hdl->infds);
	for (i = 0; i < hdl->onfds + hdl->infds; i++) {
		DPRINTF("sio_alsa_pollfd: pfds[%d].events = %x\n",
		    i, pfd[i].events);
	}
	return hdl->onfds + hdl->infds;
}

int
sio_alsa_revents(struct sio_hdl *sh, struct pollfd *pfd)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;
	snd_pcm_sframes_t idelay, odelay;
	snd_pcm_state_t istate, ostate;
	int hw_ptr, nfds;
	unsigned short revents, all_revents;
	int err;

	DPRINTF("sio_alsa_revents:\n");

	all_revents = nfds = 0;
	if (hdl->sio.mode & SIO_PLAY) {
		revents = 0;
		ostate = snd_pcm_state(hdl->out_pcm);
		if (ostate == SND_PCM_STATE_XRUN) {
			fprintf(stderr, "sio_alsa_revents: play xrun\n");
		}
		err = snd_pcm_poll_descriptors_revents(hdl->out_pcm, pfd, hdl->onfds, &revents);
		if (err < 0) {
			DALSA("snd_pcm_poll_descriptors_revents/play", err);
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		if (revents & POLLERR)
			DPRINTF("sio_alsa_revents: play POLLERR\n");
		all_revents |= revents;
		nfds += hdl->onfds;
	}
	if (hdl->sio.mode & SIO_REC) {
		revents = 0;
		istate = snd_pcm_state(hdl->in_pcm);
		if (istate == SND_PCM_STATE_XRUN) {
			printf("sio_alsa_revents: record xrun\n");
		}
		err = snd_pcm_poll_descriptors_revents(hdl->in_pcm, pfd + nfds, hdl->infds, &revents);
		if (err < 0) {
			DALSA("sio_alsa_revents: snd_pcm_poll_descriptors_revents/rec", err);
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		if (revents & POLLERR)
			DPRINTF("sio_alsa_revents: record xrun?\n");
		all_revents |= revents;
		nfds += hdl->infds;
	}
	revents = all_revents;
	DPRINTF("sio_alsa_revents: revents = %x\n", revents);

#if 0
	if ((revents & POLLOUT) && (hdl->sio.mode & SIO_PLAY) &&
	    (ostate == SND_PCM_STATE_RUNNING ||
	     ostate == SND_PCM_STATE_PREPARED)) {
		err = snd_pcm_avail_update(hdl->out_pcm);
		if (err < 0) {
			DALSA("sio_alsa_revents: play snd_pcm_avail_update", err);
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		err = snd_pcm_delay(hdl->out_pcm, &odelay);
		if (err < 0) {
			DALSA("sio_alsa_revents: play snd_pcm_delay", err);
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		if (odelay < 0) {
			printf("sio_alsa_revents: play xrun (delay)\n");
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
		err = snd_pcm_avail_update(hdl->in_pcm);
		if (err < 0) {
			DALSA("sio_alsa_revents: rec snd_pcm_avail_update", err);
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		err = snd_pcm_delay(hdl->in_pcm, &idelay);
		if (err < 0) {
			DALSA("sio_alsa_revents: record snd_pcm_delay", err);
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		if (idelay < 0) {
			printf("sio_alsa_revents: record xrun (delay)\n");
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
		if ((hdl->sio.mode & SIO_PLAY) && !sio_alsa_wsil(hdl))
			revents &= ~POLLOUT;
		if ((hdl->sio.mode & SIO_REC) && !sio_alsa_rdrop(hdl))
			revents &= ~POLLIN;
	}
#endif
	return revents;
}
#endif /* defined USE_ALSA */
