/*	$OpenBSD$	*/
/*
 * Copyright (c) 2012 Alexandre Ratchov <alex@caoua.org>
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
#include "aparams.h"
#include "defs.h"
#include "dsp.h"
#include "utils.h"

int
resamp_do(struct resamp *p, adata_t *in, adata_t *out, int todo)
{
	unsigned int nch;
	adata_t *idata;
	unsigned int oblksz;
	unsigned int ifr;
	int s, ds, diff;
	adata_t *odata;
	unsigned int iblksz;
	unsigned int ofr;
	unsigned int c;
	adata_t *ctxbuf, *ctx;
	unsigned int ctx_start;

	/*
	 * Partially copy structures into local variables, to avoid
	 * unnecessary indirections; this also allows the compiler to
	 * order local variables more "cache-friendly".
	 */
	idata = in;
	odata = out;
	diff = p->diff;
	iblksz = p->iblksz;
	oblksz = p->oblksz;
	ctxbuf = p->ctx;
	ctx_start = p->ctx_start;
	nch = p->nch;
	ifr = todo;
	ofr = oblksz;

	/*
	 * Start conversion.
	 */
#ifdef DEBUG
	if (log_level >= 4) {
		log_puts("resamp: starting diff = ");
		log_puti(diff);
		log_puts(", ifr = ");
		log_putu(ifr);
		log_puts(", ofr = ");
		log_putu(ofr);
		log_puts(" fr\n");
	}
#endif
	for (;;) {
		if (diff < 0) {
			if (ifr == 0)
				break;
			ctx_start ^= 1;
			ctx = ctxbuf + ctx_start;
			for (c = nch; c > 0; c--) {
				*ctx = *idata++;
				ctx += RESAMP_NCTX;
			}
			diff += oblksz;
			ifr--;
		} else if (diff > 0) {
			if (ofr == 0)
				break;
			ctx = ctxbuf;
			for (c = nch; c > 0; c--) {
				s = ctx[ctx_start];
				ds = ctx[ctx_start ^ 1] - s;
				ctx += RESAMP_NCTX;
				*odata++ = s + ADATA_MULDIV(ds, diff, oblksz);
			}
			diff -= iblksz;
			ofr--;
		} else {
			if (ifr == 0 || ofr == 0)
				break;
			ctx = ctxbuf + ctx_start;
			for (c = nch; c > 0; c--) {
				*odata++ = *ctx;
				ctx += RESAMP_NCTX;
			}
			ctx_start ^= 1;
			ctx = ctxbuf + ctx_start;
			for (c = nch; c > 0; c--) {
				*ctx = *idata++;
				ctx += RESAMP_NCTX;
			}
			diff -= iblksz;
			diff += oblksz;
			ifr--;
			ofr--;
		}
	}
	p->diff = diff;
	p->ctx_start = ctx_start;
	return oblksz - ofr;
}

void
resamp_init(struct resamp *p, unsigned int iblksz, unsigned int oblksz, int nch)
{
	unsigned int i;

	p->iblksz = iblksz;
	p->oblksz = oblksz;
	p->diff = 0;
	p->idelta = 0;
	p->odelta = 0;
	p->nch = nch;
	p->ctx_start = 0;
	for (i = 0; i < NCHAN_MAX * RESAMP_NCTX; i++)
		p->ctx[i] = 0;
#ifdef DEBUG
	if (log_level >= 3) {
		log_puts("resamp: init ");
		log_putu(iblksz);
		log_puts("/");
		log_putu(oblksz);
		log_puts("\n");
	}
#endif
}

void
enc_do(struct conv *p, unsigned char *in, unsigned char *out, int todo)
{
	unsigned int f;
	adata_t *idata;
	int s;
	unsigned int oshift;
	int osigbit;
	unsigned int obps;
	unsigned int i;
	unsigned char *odata;
	int obnext;
	int osnext;

#ifdef DEBUG
	if (log_level >= 4) {
		log_puts("enc: bcopy ");
		log_putu(todo);
		log_puts("\n");
	}
#endif
	/*
	 * Partially copy structures into local variables, to avoid
	 * unnecessary indirections; this also allows the compiler to
	 * order local variables more "cache-friendly".
	 */
	idata = (adata_t *)in;
	odata = out;
	oshift = p->shift;
	osigbit = p->sigbit;
	obps = p->bps;
	obnext = p->bnext;
	osnext = p->snext;

	/*
	 * Start conversion.
	 */
	odata += p->bfirst;
	for (f = todo * p->nch; f > 0; f--) {
		s = *idata++;
		s <<= 32 - ADATA_BITS;
		s >>= oshift;
		s ^= osigbit;
		for (i = obps; i > 0; i--) {
			*odata = (unsigned char)s;
			s >>= 8;
			odata += obnext;
		}
		odata += osnext;
	}
}

void
enc_sil_do(struct conv *p, unsigned char *out, int todo)
{
	unsigned int f;
	int s;
	int osigbit;
	unsigned int obps;
	unsigned int i;
	unsigned char *odata;
	int obnext;
	int osnext;

#ifdef DEBUG
	if (log_level >= 4) {
		log_puts("enc: silence ");
		log_putu(todo);
		log_puts("\n");
	}
#endif
	/*
	 * Partially copy structures into local variables, to avoid
	 * unnecessary indirections; this also allows the compiler to
	 * order local variables more "cache-friendly".
	 */
	odata = out;
	osigbit = p->sigbit;
	obps = p->bps;
	obnext = p->bnext;
	osnext = p->snext;

	/*
	 * Start conversion.
	 */
	odata += p->bfirst;
	for (f = todo * p->nch; f > 0; f--) {
		s = osigbit;
		for (i = obps; i > 0; i--) {
			*odata = (unsigned char)s;
			s >>= 8;
			odata += obnext;
		}
		odata += osnext;
	}
}

void
enc_init(struct conv *p, struct aparams *par, int nch)
{
	p->nch = nch;
	p->bps = par->bps;
	p->sigbit = par->sig ? 0 : 1 << (par->bits - 1);
	if (par->msb) {
		p->shift = 32 - par->bps * 8;
	} else {
		p->shift = 32 - par->bits;
	}
	if (!par->le) {
		p->bfirst = par->bps - 1;
		p->bnext = -1;
		p->snext = 2 * par->bps;
	} else {
		p->bfirst = 0;
		p->bnext = 1;
		p->snext = 0;
	}
#ifdef DEBUG
	if (log_level >= 3) {
		log_puts("enc_init: ");
		aparams_log(par);
		log_puts(", ");
		log_puti(p->nch);
		log_puts(" channels\n");
	}
#endif
}

void
dec_do(struct conv *p, unsigned char *in, unsigned char *out, int todo)
{
	unsigned int f;
	unsigned int ibps;
	unsigned int i;
	int s = 0xdeadbeef;
	unsigned char *idata;
	int ibnext;
	int isnext;
	int isigbit;
	unsigned int ishift;
	adata_t *odata;

#ifdef DEBUG
	if (log_level >= 4) {
		log_puts("dec: bcopy ");
		log_putu(todo);
		log_puts("\n");
	}
#endif
	/*
	 * Partially copy structures into local variables, to avoid
	 * unnecessary indirections; this also allows the compiler to
	 * order local variables more "cache-friendly".
	 */
	idata = in;
	odata = (adata_t *)out;
	ibps = p->bps;
	ibnext = p->bnext;
	isigbit = p->sigbit;
	ishift = p->shift;
	isnext = p->snext;

	/*
	 * Start conversion.
	 */
	idata += p->bfirst;
	for (f = todo * p->nch; f > 0; f--) {
		for (i = ibps; i > 0; i--) {
			s <<= 8;
			s |= *idata;
			idata += ibnext;
		}
		idata += isnext;
		s ^= isigbit;
		s <<= ishift;
		s >>= 32 - ADATA_BITS;
		*odata++ = s;
	}
}

void
dec_init(struct conv *p, struct aparams *par, int nch)
{
	p->bps = par->bps;
	p->sigbit = par->sig ? 0 : 1 << (par->bits - 1);
	p->nch = nch;
	if (par->msb) {
		p->shift = 32 - par->bps * 8;
	} else {
		p->shift = 32 - par->bits;
	}
	if (par->le) {
		p->bfirst = par->bps - 1;
		p->bnext = -1;
		p->snext = 2 * par->bps;
	} else {
		p->bfirst = 0;
		p->bnext = 1;
		p->snext = 0;
	}
#ifdef DEBUG
	if (log_level >= 3) {
		log_puts("dec_init ");
		aparams_log(par);
		log_puts(", ");
		log_puti(p->nch);
		log_puts(" channels\n");
	}
#endif
}

void
cmap_add(struct cmap *p, void *in, void *out, int vol, int todo)
{
	adata_t *idata, *odata;
	int i, j, nch, istart, inext, onext, ostart, y, v;

#ifdef DEBUG
	if (log_level >= 4) {
		log_puts("cmap: add of ");
		log_puti(todo);
		log_puts(" frames\n");
	}
#endif
	idata = in;
	odata = out;
	ostart = p->ostart;
	onext = p->onext;
	istart = p->istart;
	inext = p->inext;
	nch = p->nch;
	v = vol; /* XXX */

	/*
	 * map/mix input on the output
	 */
	for (i = todo; i > 0; i--) {
		odata += ostart;
		idata += istart;
		for (j = nch; j > 0; j--) {
			y = *odata + ADATA_MUL(*idata, v);
			if (y >= ADATA_UNIT)
				y = ADATA_UNIT - 1;
			else if (y < -ADATA_UNIT)
				y = -ADATA_UNIT;
			*odata = y;
			idata++;
			odata++;
		}
		odata += onext;
		idata += inext;
	}
}

void
cmap_copy(struct cmap *p, void *in, void *out, int vol, int todo)
{
	adata_t *idata, *odata;
	int i, j, nch, istart, inext, onext, ostart, v;

#ifdef DEBUG
	if (log_level >= 4) {
		log_puts("cmap: copy of ");
		log_puti(todo);
		log_puts(" frames\n");
	}
#endif
	idata = in;
	odata = out;
	ostart = p->ostart;
	onext = p->onext;
	istart = p->istart;
	inext = p->inext;
	nch = p->nch;
	v = vol;

	/*
	 * copy to the output buffer
	 */
	for (i = todo; i > 0; i--) {
		idata += istart;
		for (j = ostart; j > 0; j--)
			*odata++ = 0x1111;
		for (j = nch; j > 0; j--) {
			*odata = ADATA_MUL(*idata, vol);
			odata++;
			idata++;
		}
		for (j = onext; j > 0; j--)
			*odata++ = 0x2222;
		idata += inext;
	}
}

void
cmap_init(struct cmap *p,
    int imin, int imax, int isubmin, int isubmax,
    int omin, int omax, int osubmin, int osubmax)
{
	int cmin, cmax;

	cmin = -NCHAN_MAX;
	if (osubmin > cmin)
		cmin = osubmin;
	if (omin > cmin)
		cmin = omin;
	if (isubmin > cmin)
		cmin = isubmin;
	if (imin > cmin)
		cmin = imin;

	cmax = NCHAN_MAX;
	if (osubmax < cmax)
		cmax = osubmax;
	if (omax < cmax)
		cmax = omax;
	if (isubmax < cmax)
		cmax = isubmax;
	if (imax < cmax)
		cmax = imax;

	p->ostart = cmin - omin;
	p->onext = omax - cmax;
	p->istart = cmin - imin;
	p->inext = imax - cmax;
	p->nch = cmax - cmin + 1;
#ifdef DEBUG
	if (log_level >= 3) {
		log_puts("cmap_init: ");
		log_puti(isubmin);
		log_puts(":");
		log_puti(isubmax);
		log_puts(" in ");
		log_puti(imin);
		log_puts(":");
		log_puti(imax);
		log_puts(" --> ");
		log_puti(osubmin);
		log_puts(":");
		log_puti(osubmax);
		log_puts(" in ");
		log_puti(omin);
		log_puts(":");
		log_puti(omax);
		log_puts("\n");
		log_puts("cmap_init: cmin:cmax = ");
		log_puti(cmin);
		log_puts(":");
		log_puti(cmax);
		log_puts(", nch = ");
		log_puti(p->nch);
		log_puts(", ostart = ");
		log_puti(p->ostart);
		log_puts(", onext = ");
		log_puti(p->onext);
		log_puts(", istart = ");
		log_puti(p->istart);
		log_puts(", inext= ");
		log_puti(p->inext);
		log_puts("\n");
	}
#endif
}

/*
 * produce a square tone, for instance with:
 *
 *	period = round / (220 * round / rate)
 */
int
sqrtone(int ctx, adata_t *out, int period, int vol, int todo)
{
	int i;

	for (i = todo; i > 0; i--) {
		if (ctx == 0) {
			vol = -vol;
			ctx = period / 2;
		}
		ctx--;
		*(out++) = vol;
	}
	return ctx;
}
