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
	snd_pcm_t *opcm;
	snd_pcm_t *ipcm;
	unsigned ibpf, obpf;		/* bytes per frame */
	unsigned int osil;		/* frames to insert on next write */
	unsigned int idrop;		/* frames to discard on next read */
	int iused, oused;		/* frames used in hardware fifos */
	int idelta, odelta;		/* position reported to client */
	int nfds, infds, onfds;
	int running;
	int events;
	long long wpos;			/* frames written */
	long long rpos;			/* frames read */
	long long cpos;			/* hardware position (frames) */
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
		DALSA("couldn't attach to stderr", err);
#endif

	snprintf(path, sizeof(path), "hw:%s", str);
	if (mode & SIO_PLAY) {
		err = snd_pcm_open(&hdl->opcm, path,
		    SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
		if (err < 0) {
			DALSA("couldn't open play stream", err);
			goto bad_free;
		}
	}
	if (mode & SIO_REC) {
		err = snd_pcm_open(&hdl->ipcm, path,
		    SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
		if (err < 0) {
			DALSA("couldn't open rec stream", err);
			goto bad_free_opcm;
		}
	}

	/*
	 * snd_pcm_poll_descriptors_count returns a small value
	 * that grows later, after the stream is started
	 */
	hdl->nfds = SIO_MAXNFDS;
	//if (mode & SIO_PLAY)
	//	hdl->nfds += snd_pcm_poll_descriptors_count(hdl->opcm);
	//if (mode & SIO_REC)
	//	hdl->nfds += snd_pcm_poll_descriptors_count(hdl->ipcm);
	DPRINTF("mode = %d, nfds = %d\n", mode, hdl->nfds);

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

#ifdef DEBUG
void
sio_alsa_printpos(struct sio_alsa_hdl *hdl)
{	
	long long rpos, rdiff;
	long long cpos, cdiff;
	long long wpos, wdiff;

	rpos = hdl->rpos + hdl->idrop;
	wpos = hdl->wpos + hdl->osil;

	cdiff = hdl->cpos % hdl->par.round;
	cpos  = hdl->cpos / hdl->par.round;
	if (cdiff > hdl->par.round / 2) {
		cpos++;
		cdiff = cdiff - hdl->par.round;
	}

	rdiff = rpos % hdl->par.round;
	rpos  = rpos / hdl->par.round;
	if (rdiff > hdl->par.round / 2) {
		rpos++;
		rdiff = rdiff - hdl->par.round;
	}

	wdiff = wpos % hdl->par.round;
	wpos  = wpos / hdl->par.round;
	if (wdiff > hdl->par.round / 2) {
		wpos++;
		wdiff = wdiff - hdl->par.round;
	}

	//DPRINTF("iused=%d idelta=%d oused=%d, odelta=%d\n",
	//    hdl->iused, hdl->idelta, hdl->oused, hdl->odelta);
	DPRINTF("clk: %+4lld %+4lld, wr %+4lld %+4lld rd: %+4lld %+4lld\n",
	    cpos, cdiff, wpos, wdiff, rpos, rdiff);
}
#endif

static int
sio_alsa_start(struct sio_hdl *sh)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;
	int err;

	DPRINTF("sio_alsa_start:\n");

	hdl->ibpf = hdl->par.rchan * hdl->par.bps;
	hdl->obpf = hdl->par.pchan * hdl->par.bps;
	hdl->iused = 0;
	hdl->oused = 0;
	hdl->idelta = 0;
	hdl->odelta = 0;
	hdl->infds = 0;
	hdl->onfds = 0;
	hdl->osil = 0;
	hdl->idrop = 0;
	hdl->running = 0;
	hdl->cpos = hdl->rpos = hdl->wpos = 0;

	if (hdl->sio.mode & SIO_PLAY) {
		err = snd_pcm_prepare(hdl->opcm);
		if (err < 0) {
			DALSA("couldn't prepare play stream", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_prepare(hdl->ipcm);
		if (err < 0) {
			DALSA("couldn't prepare rec stream", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if ((hdl->sio.mode & SIO_PLAY) && (hdl->sio.mode & SIO_REC)) {
		err = snd_pcm_link(hdl->ipcm, hdl->opcm);
		if (err < 0) {
			DALSA("couldn't link streams", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if (!(hdl->sio.mode & SIO_PLAY)) {
		err = snd_pcm_start(hdl->ipcm);
		if (err < 0) {
			DALSA("couldn't start rec stream", err);
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
		err = snd_pcm_drop(hdl->opcm);
		if (err < 0) {
			DALSA("couldn't stop play stream", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_drop(hdl->ipcm);
		if (err < 0) {
			DALSA("couldn't stop rec stream", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	if ((hdl->sio.mode & SIO_PLAY) && (hdl->sio.mode & SIO_REC)) {
		err = snd_pcm_unlink(hdl->ipcm);
		if (err < 0) {
			DALSA("couldn't unlink streams", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
	DPRINTF("sio_alsa_stop: stopped\n");
	return 1;
}

static int
sio_alsa_xrun(struct sio_alsa_hdl *hdl)
{
	long long wpos, rpos;
	int wdiff, cdiff, rdiff;
	int wsil, rdrop, cmove;

	DPRINTF("- - - - - - - - - - - - - - - - - - - - - xrun begin\n");
	sio_alsa_printpos(hdl);

	rpos = (hdl->sio.mode & SIO_REC) ? hdl->rpos : hdl->cpos;
	wpos = (hdl->sio.mode & SIO_PLAY) ? hdl->wpos : hdl->cpos;

	cdiff = hdl->par.round - (hdl->cpos % hdl->par.round);
	if (cdiff == hdl->par.round)
		cdiff = 0;

	rdiff = rpos % hdl->par.round;

	wdiff = hdl->par.round - (wpos % hdl->par.round);
	if (wdiff == hdl->par.round)
		wdiff = 0;

	DPRINTF("rdiff = %d, cdiff = %d, wdiff = %d\n", rdiff, cdiff, wdiff);

	wsil = rdiff + wpos - rpos;
	rdrop = rdiff;
	cmove = -(rdiff + hdl->cpos - rpos);

	DPRINTF("wsil = %d, cmove = %d, rdrop = %d\n", wsil, cmove, rdrop);

	if (!sio_alsa_stop(&hdl->sio))
		return 0;
	if (!sio_alsa_start(&hdl->sio))
		return 0;

	if (hdl->sio.mode & SIO_PLAY) {
		hdl->osil = wsil;
		hdl->odelta -= cmove;
	}
	if (hdl->sio.mode & SIO_REC) {
		hdl->idrop = rdrop;
		hdl->idelta -= rdiff;
	}
	DPRINTF("xrun: corrected\n");
	DPRINTF("osil = %d, odrop = %d, odelta = %d, idelta = %d\n",
	    hdl->osil, hdl->idrop, hdl->odelta, hdl->idelta);
	sio_alsa_printpos(hdl);
	DPRINTF("- - - - - - - - - - - - - - - - - - - - - xrun end\n");
	return 1;
}

int
sio_alsa_setpar_hw(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwp,
    snd_pcm_format_t *reqfmt, unsigned int *rate, unsigned int *chans,
    snd_pcm_uframes_t *round, unsigned int *periods)
{
	static snd_pcm_format_t fmts[] = {
		SND_PCM_FORMAT_S24_LE,	SND_PCM_FORMAT_S24_BE, 
		SND_PCM_FORMAT_U24_LE,	SND_PCM_FORMAT_U24_BE,
		SND_PCM_FORMAT_S16_LE,	SND_PCM_FORMAT_S16_BE, 
		SND_PCM_FORMAT_U16_LE,	SND_PCM_FORMAT_U16_BE, 
		SND_PCM_FORMAT_U8, 	SND_PCM_FORMAT_S8
	};
	int i, err, dir = 0;
	unsigned req_rate, min_periods = 2;

	req_rate = *rate;

	err = snd_pcm_hw_params_any(pcm, hwp);
	if (err < 0) {
		DALSA("couldn't init pars", err);
		return 0;
	}
	err = snd_pcm_hw_params_set_access(pcm, hwp,
	    SND_PCM_ACCESS_RW_INTERLEAVED);
	if (err < 0) {
		DALSA("couldn't set interleaved access", err);
		return 0;
	}
	err = snd_pcm_hw_params_test_format(pcm, hwp, *reqfmt);
	if (err < 0) {
		for (i = 0; ; i++) {
			if (i == sizeof(fmts) / sizeof(snd_pcm_format_t)) {
				DPRINTF("no known format found\n");
				return 0;
			}
			err = snd_pcm_hw_params_test_format(pcm, hwp, fmts[i]);
			if (err)
				continue;
			*reqfmt = fmts[i];
			break;
		}
	}
	err = snd_pcm_hw_params_set_format(pcm, hwp, *reqfmt);
	if (err < 0) {
		DALSA("couldn't set fmt", err);
		return 0;
	}
	err = snd_pcm_hw_params_set_rate_resample(pcm, hwp, 0);
	if (err < 0) {
		DALSA("couldn't turn resampling off", err);
		return 0;
	}
	err = snd_pcm_hw_params_set_rate_near(pcm, hwp, rate, 0);
	if (err < 0) {
		DALSA("couldn't set rate", err);
		return 0;
	}
	err = snd_pcm_hw_params_set_channels_near(pcm, hwp, chans);
	if (err < 0) {
		DALSA("couldn't set channel count", err);
		return 0;
	}
	err = snd_pcm_hw_params_set_periods_integer(pcm, hwp);
	if (err < 0) {
		DALSA("couldn't set periods to integer", err);
		return 0;
	}
	err = snd_pcm_hw_params_set_periods_min(pcm, hwp, &min_periods, NULL);
	if (err < 0) {
		DALSA("couldn't set minimum periods", err);
		return 0;
	}
	err = snd_pcm_hw_params_set_period_size_integer(pcm, hwp);
	if (err < 0) {
		DALSA("couldn't set period to integer", err);
		return 0;
	}

	*round = *round * *rate / req_rate;
	*round = (*round + 31) & ~31;

	err = snd_pcm_hw_params_set_period_size_near(pcm, hwp, round, &dir);
	if (err < 0) {
		DALSA("couldn't set period size failed", err);
		return 0;
	}
	err = snd_pcm_hw_params_set_periods_near(pcm, hwp, periods, &dir);
	if (err < 0) {
		DALSA("couldn't set period count", err);
		return 0;
	}
	err = snd_pcm_hw_params(pcm, hwp);
	if (err < 0) {
		DALSA("couldn't commit params", err);
		return 0;
	}
	return 1;
}

static int
sio_alsa_setpar(struct sio_hdl *sh, struct sio_par *par)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;
	snd_pcm_hw_params_t *ohwp, *ihwp;
	snd_pcm_sw_params_t *oswp, *iswp;
	snd_pcm_uframes_t iround, oround;
	snd_pcm_format_t ifmt, ofmt;
	unsigned int iperiods, operiods;
	unsigned irate, orate;
	int err;

	/* XXX: alloca */
	snd_pcm_hw_params_malloc(&ohwp);
	snd_pcm_sw_params_malloc(&oswp);
	snd_pcm_hw_params_malloc(&ihwp);
	snd_pcm_sw_params_malloc(&iswp);

	sio_alsa_enctofmt(hdl, &ofmt, par);
	orate = (par->rate == ~0U) ? 48000 : par->rate;
	if (par->appbufsz != ~0U) {
		oround = (par->round != ~0U) ? par->round : (par->appbufsz + 1) / 2;
		operiods = par->appbufsz / oround;
		if (operiods < 2)
			operiods = 2;
	} else if (par->round != ~0U) {
		oround = par->round;
		operiods = 2;
	} else {
		operiods = 2;
		oround = orate / 100;
	}

	if (hdl->sio.mode & SIO_PLAY) {
		hdl->par.pchan = par->pchan;
		if (!sio_alsa_setpar_hw(hdl->opcm, ohwp,
			&ofmt, &orate, &hdl->par.pchan,
			&oround, &operiods)) {
			hdl->sio.eof = 1;
			return 0;
		}
	}
	ifmt = ofmt;
	irate = orate;
	iround = oround;
	iperiods = operiods;
	if (hdl->sio.mode & SIO_REC) {
		hdl->par.rchan = par->rchan;
		if (!sio_alsa_setpar_hw(hdl->ipcm, ihwp,
			&ifmt, &irate, &par->rchan,
			&iround, &iperiods)) {
			hdl->sio.eof = 1;
			return 0;
		}
		if (!(hdl->sio.mode & SIO_PLAY)) {
			ofmt = ifmt;
			orate = irate;
			iround = oround;
			iperiods = operiods;
		}
	}

	DPRINTF("ofmt = %u, orate = %u, oround = %u, operiods = %u\n",
	    ofmt, orate, (unsigned int)oround, operiods);
	
	if (ifmt != ofmt) {
		DPRINTF("play and rec formats differ\n");
		hdl->sio.eof = 1;
		return 0;
	}
	if (irate != orate) {
		DPRINTF("play and rec rates differ\n");
		hdl->sio.eof = 1;
		return 0;
	}
	if (iround != oround) {
		DPRINTF("play and rec block sizes differ\n");
		hdl->sio.eof = 1;
		return 0;
	}
	if (!sio_alsa_fmttopar(hdl, ofmt, &hdl->par))
		return 0;
	hdl->par.rate = orate;
	hdl->par.round = oround;
	hdl->par.bufsz = oround * operiods;
	hdl->par.appbufsz = hdl->par.bufsz;

	/* software params */

	if (hdl->sio.mode & SIO_PLAY) {
		err = snd_pcm_sw_params_current(hdl->opcm, oswp);
		if (err < 0) {
			DALSA("couldn't get current play params", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_start_threshold(hdl->opcm,
		    oswp, hdl->par.bufsz);
		if (err < 0) {
			DALSA("couldn't set play start threshold", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_stop_threshold(hdl->opcm,
		    oswp, hdl->par.bufsz);
		if (err < 0) {
			DALSA("couldn't set play stop threshold", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_avail_min(hdl->opcm,
		    oswp, hdl->par.round);
		if (err < 0) {
			DALSA("couldn't set play avail min", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_period_event(hdl->opcm, oswp, 1);
		if (err < 0) {
			DALSA("couldn't set play period event", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params(hdl->opcm, oswp);
		if (err < 0) {
			DALSA("couldn't commit play sw params", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}

	if (hdl->sio.mode & SIO_REC) {
		err = snd_pcm_sw_params_current(hdl->ipcm, iswp);
		if (err < 0) {
			DALSA("couldn't get current rec params", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_start_threshold(hdl->ipcm,
		    iswp, 0);
		if (err < 0) {
			DALSA("couldn't set rec start threshold", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_stop_threshold(hdl->ipcm,
		    iswp, hdl->par.bufsz);
		if (err < 0) {
			DALSA("couldn't set rec stop threshold", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_avail_min(hdl->ipcm,
		    iswp, hdl->par.round);
		if (err < 0) {
			DALSA("couldn't set rec avail min", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params_set_period_event(hdl->ipcm, iswp, 1);
		if (err < 0) {
			DALSA("couldn't set rec period event", err);
			hdl->sio.eof = 1;
			return 0;
		}
		err = snd_pcm_sw_params(hdl->ipcm, iswp);
		if (err < 0) {
			DALSA("couldn't commit rec sw params", err);
			hdl->sio.eof = 1;
			return 0;
		}
	}
#ifdef DEBUG
	if (sndio_debug) {
		if (hdl->sio.mode & SIO_REC)
			snd_pcm_dump(hdl->ipcm, output);
		if (hdl->sio.mode & SIO_PLAY)
			snd_pcm_dump(hdl->opcm, output);
	}
#endif
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
	static char buf[DROP_NMAX];
	ssize_t n, todo, max;

	if (hdl->idrop == 0)
		return 1;

	max = DROP_NMAX / hdl->ibpf;
	while (hdl->idrop > 0) {
		todo = hdl->idrop;
		if (todo > max)
			todo = max;
		while ((n = snd_pcm_readi(hdl->ipcm, buf, todo)) < 0) {
			if (n == -EINTR)
				continue;
			if (n == -EPIPE || n == -ESTRPIPE) {
				sio_alsa_xrun(hdl);
				return 0;
			}
			if (n != -EAGAIN) {
				DALSA("couldn't read data to drop", n);
				hdl->sio.eof = 1;
			}
			return 0;
		}
		if (n == 0) {
			DPRINTF("sio_alsa_rdrop: eof\n");
			hdl->sio.eof = 1;
			return 0;
		}
		hdl->idrop -= n;
		hdl->rpos += n;

		/*
		 * dropping samples is clock-wise neutral, so we
		 * should not bump hdl->idelta += n; but since
		 * we dropped samples, kernel buffer usage changed
		 * and we have to take this into account here, to
		 * prevent sio_alsa_revents() interpretin iused
		 * change as clock tick
		 */
		hdl->iused -= n;
		DPRINTF("sio_alsa_rdrop: dropped %zu/%zu frames\n", n, todo);
	}
	return 0;
}

static size_t
sio_alsa_read(struct sio_hdl *sh, void *buf, size_t len)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;
	snd_pcm_sframes_t n;
	snd_pcm_uframes_t todo;

	todo = len / hdl->ibpf;
	while ((n = snd_pcm_readi(hdl->ipcm, buf, todo)) < 0) {
		if (n == -EINTR)
			continue;
		if (n == -EPIPE || n == -ESTRPIPE) {
			sio_alsa_xrun(hdl);
			return 0;
		}
		if (n != -EAGAIN) {
			DALSA("couldn't read data", n);
			hdl->sio.eof = 1;
		}
		return 0;
	}
	if (n == 0) {
		DPRINTF("sio_alsa_read: eof\n");
		hdl->sio.eof = 1;
		return 0;
	}
	hdl->rpos += n;
	hdl->idelta += n;
	n *= hdl->ibpf;
	return n;
}

/*
 * insert silence to play to compensate xruns
 */
static int
sio_alsa_wsil(struct sio_alsa_hdl *hdl)
{
#define ZERO_NMAX 0x10000
	static char zero[ZERO_NMAX];
	ssize_t n, todo, max;

	if (hdl->osil == 0)
		return 1;

	max = ZERO_NMAX / hdl->obpf;
	while (hdl->osil > 0) {
		todo = hdl->osil;
		if (todo > max)
			todo = max;
		while ((n = snd_pcm_writei(hdl->opcm, zero, todo)) < 0) {
			if (n == -EINTR)
				continue;
			if (n == -ESTRPIPE || n == -EPIPE) {
				sio_alsa_xrun(hdl);
				return 0;
			}
			if (n != -EAGAIN) {
				DALSA("couldn't write silence", n);
				hdl->sio.eof = 1;
			}
			return 0;
		}
#ifdef DEBUG
		hdl->wpos += n;
#endif
		/*
		 * be clock-wise neutral, see end of sio_alsa_wsil()
		 */
		hdl->oused += n;
		hdl->osil -= n;
		DPRINTF("sio_alsa_wsil: inserted %zu/%zu frames\n", n, todo);
	}
	return 0;
}

static size_t
sio_alsa_write(struct sio_hdl *sh, const void *buf, size_t len)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;
	ssize_t n, todo;

	if (hdl->osil) {
		if (!sio_alsa_wsil(hdl))
			return 0;
		return 0;
	}
	if (len < hdl->obpf) {
		/*
		 * we can't just return, because sio_write() will loop
		 * forever. Fix this by saving partial samples in a
		 * temporary buffer.
		 */
		DPRINTF("sio_alsa_write: wrong chunk size\n");
		hdl->sio.eof = 1;
		return 0;
	}
	todo = len / hdl->obpf;
	DPRINTFN(2, "sio_alsa_write: len = %zd, todo = %zd\n", len, todo);
	while ((n = snd_pcm_writei(hdl->opcm, buf, todo)) < 0) {
		if (n == -EINTR)
			continue;
		if (n == -ESTRPIPE || n == -EPIPE) {
			sio_alsa_xrun(hdl);
			return 0;
		}
		if (n != -EAGAIN) {
			DALSA("couldn't write data", n);
			hdl->sio.eof = 1;
		}
		return 0;
	}
	DPRINTFN(2, "sio_alsa_write: wrote %zd\n", n);
#ifdef DEBUG
	hdl->wpos += n;
#endif
	hdl->odelta += n;
	n *= hdl->obpf;
	return n;
}

void
sio_alsa_onmove(struct sio_alsa_hdl *hdl)
{
	int delta;

	if (hdl->sio.mode & SIO_PLAY)
		DPRINTF("ostate = %d\n", snd_pcm_state(hdl->opcm));
	if (hdl->sio.mode & SIO_REC)
		DPRINTF("istate = %d\n", snd_pcm_state(hdl->ipcm));

	if (hdl->running) {
		switch (hdl->sio.mode & (SIO_PLAY | SIO_REC)) {
		case SIO_PLAY:
			delta = hdl->odelta;
			break;
		case SIO_REC:
			delta = hdl->idelta;
			break;
		case SIO_PLAY | SIO_REC:
			delta = hdl->odelta > hdl->idelta ?
				hdl->odelta : hdl->idelta;
			break;
		}
		if (delta <= 0)
			return;
	} else {
		delta = 0;
		hdl->running = 1;
	}
#ifdef DEBUG
	hdl->cpos += delta;
	if (sndio_debug >= 1)
		sio_alsa_printpos(hdl);
#endif
	sio_onmove_cb(&hdl->sio, delta);
	if (hdl->sio.mode & SIO_PLAY)
		hdl->odelta -= delta;
	if (hdl->sio.mode & SIO_REC)
		hdl->idelta -= delta;
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

	if (hdl->sio.eof)
		return 0;

	hdl->events = events;
	memset(pfd, 0, sizeof(struct pollfd) * hdl->nfds);
	if ((events & POLLOUT) && (hdl->sio.mode & SIO_PLAY) &&
	    hdl->sio.started) {
		if (!hdl->running &&
		    snd_pcm_state(hdl->opcm) == SND_PCM_STATE_RUNNING)
			sio_alsa_onmove(hdl);
		hdl->onfds = snd_pcm_poll_descriptors(hdl->opcm,
		    pfd, hdl->nfds);
		if (hdl->onfds < 0) {
			DALSA("couldn't poll play descriptors", hdl->onfds);
			hdl->sio.eof = 1;
			return 0;
		}
	} else
		hdl->onfds = 0;
	if ((events & POLLIN) && (hdl->sio.mode & SIO_REC) &&
	    hdl->sio.started) {
		if (!hdl->running &&
		    snd_pcm_state(hdl->ipcm) == SND_PCM_STATE_RUNNING)
			sio_alsa_onmove(hdl);
		hdl->infds = snd_pcm_poll_descriptors(hdl->ipcm,
		    pfd + hdl->onfds, hdl->nfds - hdl->onfds);
		if (hdl->infds < 0) {
			DALSA("couldn't poll rec descriptors", hdl->infds);
			hdl->sio.eof = 1;
			return 0;
		}
	} else
		hdl->infds = 0;
	DPRINTFN(2, "sio_alsa_pollfd: events = %x, nfds = %d + %d\n",
	    events, hdl->onfds, hdl->infds);

	for (i = 0; i < hdl->onfds + hdl->infds; i++) {
		DPRINTFN(3, "sio_alsa_pollfd: pfds[%d].events = %x\n",
		    i, pfd[i].events);
	}
	return hdl->onfds + hdl->infds;
}

int
sio_alsa_revents(struct sio_hdl *sh, struct pollfd *pfd)
{
	struct sio_alsa_hdl *hdl = (struct sio_alsa_hdl *)sh;
	snd_pcm_sframes_t iused, oavail, oused;
	snd_pcm_state_t istate, ostate;
	unsigned short revents, r;
	int nfds, err, i;

	if (hdl->sio.eof)
		return POLLHUP;

	for (i = 0; i < hdl->onfds + hdl->infds; i++) {
		DPRINTFN(3, "sio_alsa_revents: pfds[%d].events = %x\n",
		    i, pfd[i].revents);
	}
	revents = nfds = 0;
	if ((hdl->events & POLLOUT) && (hdl->sio.mode & SIO_PLAY)) {
		ostate = snd_pcm_state(hdl->opcm);
		if (ostate == SND_PCM_STATE_XRUN) {
			if (!sio_alsa_xrun(hdl))
				return POLLHUP;
			return 0;
		}
		err = snd_pcm_poll_descriptors_revents(hdl->opcm,
		    pfd, hdl->onfds, &r);
		if (err < 0) {
			DALSA("couldn't get play events", err);
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		revents |= r;
		nfds += hdl->onfds;
			
	}
	if ((hdl->events & POLLIN) && (hdl->sio.mode & SIO_REC)) {
		istate = snd_pcm_state(hdl->ipcm);
		if (istate == SND_PCM_STATE_XRUN) {
			if (!sio_alsa_xrun(hdl))
				return POLLHUP;
			return 0;
		}
		err = snd_pcm_poll_descriptors_revents(hdl->ipcm,
		    pfd + nfds, hdl->infds, &r);
		if (err < 0) {
			DALSA("couldn't get rec events", err);
			hdl->sio.eof = 1;
			return POLLHUP;
		}
		revents |= r;
		nfds += hdl->infds;
	}
	DPRINTFN(2, "sio_alsa_revents: revents = %x\n", revents);
	if (revents & (POLLIN | POLLOUT)) {
		if ((hdl->sio.mode & SIO_PLAY) &&
		    (ostate == SND_PCM_STATE_RUNNING ||
			ostate == SND_PCM_STATE_PREPARED)) {
			oavail = snd_pcm_avail_update(hdl->opcm);
			if (oavail < 0) {
				if (oavail == -EPIPE || oavail == -ESTRPIPE) {
					if (!sio_alsa_xrun(hdl))
						return POLLHUP;
					return 0;
				}
				DALSA("couldn't get play buffer ptr", oavail);
				hdl->sio.eof = 1;
				return POLLHUP;
			}
			oused = hdl->par.bufsz - oavail;
			hdl->odelta -= oused - hdl->oused;
			hdl->oused = oused;
		}
		if ((hdl->sio.mode & SIO_REC) &&
		    (istate == SND_PCM_STATE_RUNNING ||
			istate == SND_PCM_STATE_PREPARED)) {
			iused = snd_pcm_avail_update(hdl->ipcm);
			if (iused < 0) {
				if (iused == -EPIPE || iused == -ESTRPIPE) {
					if (!sio_alsa_xrun(hdl))
						return POLLHUP;
					return 0;
				}
				DALSA("couldn't get rec buffer ptr", iused);
				hdl->sio.eof = 1;
				return POLLHUP;
			}
			hdl->idelta += iused - hdl->iused;
			hdl->iused = iused;
		}
		if (hdl->running ||
		    ((hdl->sio.mode & SIO_PLAY) &&
		     ostate == SND_PCM_STATE_RUNNING) ||
		    ((hdl->sio.mode & SIO_REC) &&
		     istate == SND_PCM_STATE_RUNNING))
			sio_alsa_onmove(hdl);
	}
	if ((hdl->sio.mode & SIO_PLAY) && !sio_alsa_wsil(hdl))
		revents &= ~POLLOUT;
	if ((hdl->sio.mode & SIO_REC) && !sio_alsa_rdrop(hdl))
		revents &= ~POLLIN;
	return revents;
}
#endif /* defined USE_ALSA */
