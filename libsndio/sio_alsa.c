/*	$OpenBSD$	*/
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

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <values.h>
#include <alsa/asoundlib.h>

#include "debug.h"
#include "sio_priv.h"
#include "bsd-compat.h"

#ifdef DEBUG
static snd_output_t *output = NULL;
#define DALSA(str, err) fprintf(stderr, "%s: %s\n", str, snd_strerror(err)) 
#else
#define DALSA(str, err) do {} while (0)
#endif

struct sio_alsa_hdl {
	struct sio_hdl sio;
	struct sio_par par;
	struct pollfd *pfds;
	snd_pcm_t *opcm;
	snd_pcm_t *ipcm;
	int filling, filltodo;
	unsigned obufsz, ibufsz;	/* frames in the buffer */
	unsigned ibpf, obpf;		/* bytes per frame */
	unsigned ihfr, ohfr;		/* frames the hw transfered */
	unsigned isfr, osfr;		/* frames the sw transfered */
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
	NULL,
	NULL
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
		if (snd_pcm_hw_params_test_format(hdl->opcm, hdl->out_hwp,
		    fmt) < 0)
			return 0;
	}
	if (hdl->sio.mode & SIO_REC) {
		if (snd_pcm_hw_params_test_format(hdl->ipcm, hdl->in_hwp,
		    fmt) < 0)
			return 0;
	}

	if (pchan && (hdl->sio.mode & SIO_PLAY)) {
		if (snd_pcm_hw_params_test_channels(hdl->opcm, hdl->out_hwp,
		    pchan) < 0)
			return 0;
	}
	if (rchan && (hdl->sio.mode & SIO_REC)) {
		if (snd_pcm_hw_params_test_channels(hdl->ipcm, hdl->in_hwp,
		    rchan) < 0)
			return 0;
	}

	if (rate && (hdl->sio.mode & SIO_PLAY)) {
		if (snd_pcm_hw_params_test_rate(hdl->opcm, hdl->out_hwp,
		    rate, 0) < 0)
			return 0;
	}
	if (rate && (hdl->sio.mode & SIO_REC)) {
		if (snd_pcm_hw_params_test_rate(hdl->ipcm, hdl->in_hwp,
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

struct sio_hdl *
sio_alsa_open(const char *str, unsigned mode, int nbio)
{
	struct sio_alsa_hdl *hdl;
	char path[PATH_MAX];
	struct sio_par par;
	int err;

	switch (*str) {
	case '/':
	case ':': /* XXX: for backward compat */
		str++;
		break;
	default:
		DPRINTF("sio_sun_open: %s: '/<devnum>' expected\n", str);
		return NULL;
	}
	hdl = malloc(sizeof(struct sio_alsa_hdl));
	if (hdl == NULL)
		return NULL;
	sio_create(&hdl->sio, &sio_alsa_ops, mode, nbio);

#ifdef DEBUG
	err = snd_output_stdio_attach(&output, stderr, 0);
	if (err < 0)
		DALSA("attach to stderr", err);
#endif

	snprintf(path, sizeof(path), "hw:%s", str);
	if (mode & SIO_PLAY) {
		err = snd_pcm_open(&hdl->opcm, path,
		    SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
		if (err < 0) {
			DALSA("snd_pcm_open", err);
			goto bad_free;
		}
	}
	if (mode & SIO_REC) {
		err = snd_pcm_open(&hdl->ipcm, path,
		    SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
		if (err < 0) {
			DALSA("snd_pcm_open", err);
			goto bad_free_opcm;
		}
	}
	hdl->nfds = SIO_MAXNFDS;
	hdl->pfds = malloc(sizeof(struct pollfd) * hdl->nfds);
	if (hdl->pfds == NULL) {
		DPERROR("couldn't allocate pollfd structures");
		goto bad_free_ipcm;
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
		goto bad_free_ipcm;
	return (struct sio_hdl *)hdl;
bad_free_ipcm:
	if (mode & SIO_REC)
		snd_pcm_close(hdl->ipcm);
bad_free_opcm:
	if (mode & SIO_PLAY)
		snd_pcm_close(hdl->opcm);
bad_free:
	free(hdl);
	return NULL;
}

static void
sio_alsa_close(struct sio_hdl *sh)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;

	if (hdl->sio.mode & SIO_PLAY)
		snd_pcm_close(hdl->opcm);
	if (hdl->sio.mode & SIO_REC)
		snd_pcm_close(hdl->ipcm);
	free(hdl);
}

static int
sio_alsa_start(struct sio_hdl *sh)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;
	snd_pcm_sframes_t idelay;
	int err;

	DPRINTF("sio_alsa_start:\n");

	hdl->ibpf = hdl->par.rchan * hdl->par.bps;
	hdl->obpf = hdl->par.pchan * hdl->par.bps;
	hdl->ihfr = 0;
	hdl->ohfr = 0;
	hdl->isfr = 0;
	hdl->osfr = 0;
	hdl->offset = 0;
	hdl->idelta = 0;
	hdl->odelta = 0;
	hdl->infds = 0;
	hdl->onfds = 0;

	if (hdl->sio.mode & SIO_PLAY) {
		err = snd_pcm_prepare(hdl->opcm);
		if (err < 0) {
			DALSA("sio_alsa_start: prepare playback failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_prepare(hdl->ipcm);
		if (err < 0) {
			DALSA("sio_alsa_start: prepare record failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if ((hdl->sio.mode & SIO_PLAY) && (hdl->sio.mode & SIO_REC)) {
		err = snd_pcm_link(hdl->ipcm, hdl->opcm);
		if (err < 0) {
			DALSA("could not link", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	hdl->filling = 0;
	if (hdl->sio.mode & SIO_PLAY) {
		hdl->filling = 1;
		hdl->filltodo = hdl->par.bufsz;
	} else {
		idelay = snd_pcm_avail(hdl->ipcm);
		fprintf(stderr, "start 1 : ielay = %u\n", idelay);
		err = snd_pcm_start(hdl->ipcm);
		if (err < 0) {
			DALSA("sio_alsa_start: start record failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		usleep(1000);
		idelay = snd_pcm_avail(hdl->ipcm);
		fprintf(stderr, "start 2 : ielay = %u\n", idelay);
		sio_onmove_cb(&hdl->sio, 0);
	}
	//snd_pcm_dump(hdl->ipcm, output);
	return 1;
}

static int
sio_alsa_stop(struct sio_hdl *sh)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;
	int err;

	if (hdl->sio.mode & SIO_PLAY) {
		err = snd_pcm_drop(hdl->opcm);
		if (err < 0) {
			DALSA("sio_alsa_stop: drop/close playback failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_drop(hdl->ipcm);
		if (err < 0) {
			DALSA("sio_alsa_stop: drop/close record failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if ((hdl->sio.mode & SIO_PLAY) && (hdl->sio.mode & SIO_REC)) {
		err = snd_pcm_unlink(hdl->ipcm);
		if (err < 0) {
			DALSA("could not unlink in", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	DPRINTF("stopped");
	return 1;
}

static int
sio_alsa_setpar(struct sio_hdl *sh, struct sio_par *par)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;
	snd_pcm_hw_params_t *ohwp, *ihwp;
	snd_pcm_sw_params_t *oswp, *iswp;
	snd_pcm_uframes_t infr, onfr, ibufsz, obufsz;
	snd_pcm_format_t ifmt, ofmt;
	unsigned bufsz, round;
	unsigned irate, orate, req_rate;
	unsigned ich, och;
	int err;

	snd_pcm_hw_params_malloc(&ohwp);
	snd_pcm_sw_params_malloc(&oswp);
	snd_pcm_hw_params_malloc(&ihwp);
	snd_pcm_sw_params_malloc(&iswp);

	/*
	 * set encoding
	 */
	sio_alsa_enctofmt(hdl, &ofmt, par);
	DPRINTF("ofmt = %u\n", ofmt);
	if (hdl->sio.mode & SIO_PLAY) {
		err = snd_pcm_hw_params_any(hdl->opcm, ohwp);
		if (err < 0) {
			DALSA("couldn't init play pars", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_hw_params_set_access(hdl->opcm, ohwp,
		    SND_PCM_ACCESS_RW_INTERLEAVED);
		if (err < 0) {
			DALSA("couldn't set play access", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_hw_params_set_format(hdl->opcm, ohwp, ofmt);
		if (err < 0) {
			DALSA("couldn't set play fmt", err);
			hdl->sio.eof = 1;
			/*
			 * XXX: try snd_pcm_set_format_mask
			 */
			return 0;
		}
		err = snd_pcm_hw_params_get_format(ohwp, &ofmt);
		if (err < 0) {
			DALSA("couldn't get play fmt", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	ifmt = ofmt;
	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_hw_params_any(hdl->ipcm, ihwp);
		if (err < 0) {
			DALSA("couldn't init rec pars", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_hw_params_set_access(hdl->ipcm, ihwp,
		    SND_PCM_ACCESS_RW_INTERLEAVED);
		if (err < 0) {
			DALSA("couldn't set rec access", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_hw_params_set_format(hdl->ipcm, ihwp, ifmt);
		if (err < 0) {
			DALSA("couldn't set rec fmt", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_hw_params_get_format(ihwp, &ifmt);
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
		err = snd_pcm_hw_params_set_rate_near(hdl->opcm,
		    ohwp, &orate, 0);
		if (err < 0) {
			DALSA("couldn't set play rate", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	irate = orate;
	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_hw_params_set_rate_near(hdl->ipcm,
		    ihwp, &irate, 0);
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
		err = snd_pcm_hw_params_set_channels_near(hdl->opcm,
		    ohwp, &och);
		if (err < 0) {
			DALSA("set play channel count failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		hdl->par.pchan = och;
	}
	if ((hdl->sio.mode & SIO_REC) && par->rchan != ~0U) {
		ich = par->rchan;
		err = snd_pcm_hw_params_set_channels_near(hdl->ipcm,
		    ihwp, &ich);
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
		err = snd_pcm_hw_params_set_buffer_size_near(hdl->opcm,
		    ohwp, &obufsz);
		if (err < 0) {
			DALSA("set play bufsz failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		DPRINTF("sio_alsa_setpar: obufsz: %u, ok\n", obufsz);
	}
	ibufsz = obufsz;
	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_hw_params_set_buffer_size_near(hdl->ipcm,
		    ihwp, &ibufsz);
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
	hdl->obufsz = obufsz;
	hdl->ibufsz = ibufsz;

	onfr = round;
	if (hdl->sio.mode & SIO_PLAY) {
		err = snd_pcm_hw_params_set_period_size_near(hdl->opcm,
		    ohwp, &onfr, NULL);
		if (err < 0) {
			DALSA("set play period size failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		DPRINTF("sio_alsa_setpar: onfr: %u, ok\n", onfr);
	}
	infr = onfr;
	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_hw_params_set_period_size_near(hdl->ipcm,
		    ihwp, &infr, NULL);
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
		err = snd_pcm_hw_params(hdl->opcm, ohwp);
		if (err < 0) {
			DALSA("commit play params failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		DPRINTF("sio_alsa_setpar: out_hwp: ok\n");

	}
	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_hw_params(hdl->ipcm, ihwp);
		if (err < 0) {
			DALSA("commit record params failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}

	/* software params */

	if (hdl->sio.mode & SIO_PLAY) {
		err = snd_pcm_sw_params_current(hdl->opcm, oswp);
		if (err < 0) {
			DALSA("snd_pcm_sw_params_current", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_start_threshold(hdl->opcm,
		    oswp, INT_MAX);
		if (err < 0) {
			DALSA("set play start threshold failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_avail_min(hdl->opcm,
		    oswp, 1);
		if (err < 0) {
			DALSA("set play avail min failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_period_event(hdl->opcm, oswp, 1);
		if (err < 0) {
			DALSA("can't set period event", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params(hdl->opcm, oswp);
		if (err < 0) {
			DALSA("commit play sw params failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}

	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_sw_params_current(hdl->ipcm, iswp);
		if (err < 0) {
			DALSA("snd_pcm_sw_params_current", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_start_threshold(hdl->ipcm,
		    iswp, INT_MAX);
		if (err < 0) {
			DALSA("set record start threshold failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_avail_min(hdl->ipcm,
		    iswp, infr);
		if (err < 0) {
			DALSA("set rec avail min failed", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_period_event(hdl->ipcm, iswp, 1);
		if (err < 0) {
			DALSA("can't set period event", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params(hdl->ipcm, iswp);
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
		while ((n = snd_pcm_readi(hdl->ipcm, dropbuf, todo)) < 0) {
			if (n == -EINTR)
				continue;
			if (n != -EAGAIN) {
				DALSA("sio_alsa_rdrop: readi", n);
				hdl->sio.eof = 1;
			}
			return 0;
		}
		if (n == 0) {
			DPRINTF("sio_alsa_rdrop: eof\n");
			hdl->sio.eof = 1;
			return 0;
		}
		hdl->offset -= (int)n;
		hdl->isfr += (int)n;
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
	while ((n = snd_pcm_readi(hdl->ipcm, buf, todo)) < 0) {
		if (n == -EINTR)
			continue;
		//if (n == -ESTRPIPE)
		//	continue;
		if (n != -EAGAIN) {
			DALSA("sio_alsa_read: read", n);
			hdl->sio.eof = 1;
		}
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
	snd_pcm_sframes_t odelay;


	odelay = snd_pcm_avail(hdl->opcm);
	fprintf(stderr, "start 1 : delay = %u\n", hdl->obufsz - odelay);

	state = snd_pcm_state(hdl->opcm);
	if (state == SND_PCM_STATE_PREPARED) {
		err = snd_pcm_start(hdl->opcm);
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

	usleep(1000);

	odelay = snd_pcm_avail(hdl->opcm);
	fprintf(stderr, "start 2 : delay = %u\n", hdl->obufsz - odelay);
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
		if ((n = snd_pcm_writei(hdl->opcm, zero, todo)) < 0) {
			if (n == -EINTR)
				continue;
			if (n != -EAGAIN) {
				DALSA("sio_alsa_wsil", n);
				hdl->sio.eof = 1;
			}
			return 0;
		}
		hdl->offset += (int)n;
		hdl->osfr += (int)n;
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
	while ((n = snd_pcm_writei(hdl->opcm, buf, todo)) < 0) {
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
	int i, nfds = 0;

	/* XXX: check that SIO_MAXNFDS is ok */

	DPRINTF("sio_alsa_pollfd: count = %d, nfds = %d\n", 
	    snd_pcm_poll_descriptors_count(hdl->opcm),
	    hdl->nfds);

	memset(pfd, 0, sizeof(struct pollfd) * hdl->nfds);
	if ((events & POLLOUT) && (hdl->sio.mode & SIO_PLAY) && hdl->sio.started) {
		hdl->onfds = snd_pcm_poll_descriptors(hdl->opcm,
		    pfd, hdl->nfds);
		if (hdl->onfds < 0) {
			DALSA("poll out descriptors", hdl->onfds);
			hdl->sio.eof = 1;
			return 0;
		}
	} else
		hdl->onfds = 0;
	if ((events & POLLIN) && (hdl->sio.mode & SIO_REC) && hdl->sio.started) {
		hdl->infds = snd_pcm_poll_descriptors(hdl->ipcm,
		    pfd + hdl->onfds, hdl->nfds - hdl->onfds);
		if (hdl->infds < 0) {
			DALSA("poll in descriptors", hdl->infds);
			hdl->sio.eof = 1;
			return 0;
		}
	} else
		hdl->infds = 0;
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
	unsigned short revents, r;
	int i, err;

	for (i = 0; i < hdl->onfds + hdl->infds; i++) {
		DPRINTF("sio_alsa_revents: pfds[%d].events = %x\n",
		    i, pfd[i].revents);
	}

	revents = nfds = 0;
	if (hdl->sio.mode & SIO_PLAY) {
		ostate = snd_pcm_state(hdl->opcm);
		if (ostate == SND_PCM_STATE_XRUN) {
			fprintf(stderr, "sio_alsa_revents: play xrun\n");
		}
		err = snd_pcm_poll_descriptors_revents(hdl->opcm, pfd, hdl->onfds, &r);
		if (err < 0) {
			DALSA("snd_pcm_poll_descriptors_revents/play", err);
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		if (r & POLLERR)
			DPRINTF("sio_alsa_revents: play POLLERR\n");
		revents |= r;
		nfds += hdl->onfds;
	}
	if (hdl->sio.mode & SIO_REC) {
		istate = snd_pcm_state(hdl->ipcm);
		if (istate == SND_PCM_STATE_XRUN) {
			fprintf(stderr, "sio_alsa_revents: record xrun\n");
		}
		err = snd_pcm_poll_descriptors_revents(hdl->ipcm, pfd + nfds, hdl->infds, &r);
		if (err < 0) {
			DALSA("sio_alsa_revents: snd_pcm_poll_descriptors_revents/rec", err);
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		if (r & POLLERR)
			DPRINTF("sio_alsa_revents: record xrun?\n");
		revents |= r;
		nfds += hdl->infds;
	}
	DPRINTF("sio_alsa_revents: revents = %x\n", revents);
	if ((revents & POLLOUT) && (hdl->sio.mode & SIO_PLAY) &&
	    (ostate == SND_PCM_STATE_RUNNING ||
	     ostate == SND_PCM_STATE_PREPARED)) {
#if 1
		odelay = snd_pcm_avail(hdl->opcm);
		if (odelay < 0) {
			DALSA("sio_alsa_revents: play snd_pcm_avail_update", odelay);
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		odelay = hdl->obufsz - odelay;
#endif
#if 0		
		err = snd_pcm_delay(hdl->opcm, &odelay);
		if (err < 0) {
			DALSA("sio_alsa_revents: play snd_pcm_delay", err);
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		if (odelay < 0) {
			fprintf(stderr, "sio_alsa_revents: negative odelay %d\n", odelay);
		}
#endif
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
		idelay = snd_pcm_avail(hdl->ipcm);
		if (idelay < 0) {
			DALSA("sio_alsa_revents: rec snd_pcm_avail_update", idelay);
			hdl->sio.eof = 1;
			return POLLHUP;
		}
#if 0
		err = snd_pcm_delay(hdl->ipcm, &idelay);
		if (err < 0) {
			DALSA("sio_alsa_revents: record snd_pcm_delay", err);
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		if (idelay < 0) {
			fprintf(stderr, "sio_alsa_revents: negative idelay %d\n", idelay);
		}
#endif
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
	return revents;
}
#endif /* defined USE_ALSA */
