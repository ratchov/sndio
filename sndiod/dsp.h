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
#ifndef DSP_H
#define DSP_H

#include "aparams.h"

struct resamp {
	void *data;
#define RESAMP_NCTX	2
	unsigned int ctx_start;
	adata_t ctx[NCHAN_MAX * RESAMP_NCTX];
	unsigned int iblksz, oblksz;
	int diff;
	int idelta, odelta;			/* remainder of ipos/opos */
	int nch;
};

struct conv {
	void *data;
	int bfirst;				/* bytes to skip at startup */
	unsigned int bps;			/* bytes per sample */
	unsigned int shift;			/* shift to get 32bit MSB */
	int sigbit;				/* sign bits to XOR */
	int bnext;				/* to reach the next byte */
	int snext;				/* to reach the next sample */
	int nch;
};

struct cmap {
	int istart;
	int inext;
	int onext;
	int ostart;
	int nch;
};

int resamp_do(struct resamp *, adata_t *, adata_t *, int);
void resamp_init(struct resamp *, unsigned int, unsigned int, int);
void enc_do(struct conv *, unsigned char *, unsigned char *, int);
void enc_sil_do(struct conv *, unsigned char *, int);
void enc_init(struct conv *, struct aparams *, int);
void dec_do(struct conv *, unsigned char *, unsigned char *, int);
void dec_init(struct conv *, struct aparams *, int);
void cmap_add(struct cmap *, void *, void *, int, int);
void cmap_copy(struct cmap *, void *, void *, int, int);
void cmap_init(struct cmap *, int, int, int, int, int, int, int, int);
int sqrtone(int, adata_t *, int, int, int);

#endif /* !defined(DSP_H) */
