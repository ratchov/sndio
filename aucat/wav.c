/*
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

#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "utils.h"
#include "wav.h"

/*
 * Max size of a .wav file, format design limitation.
 */
#define WAV_MAXPOS	(0x7fffffff)

/*
 * Encoding IDs used in .wav headers.
 */
#define WAV_ENC_PCM	1
#define WAV_ENC_ALAW	6
#define WAV_ENC_ULAW	7
#define WAV_ENC_EXT	0xfffe

typedef struct {
	unsigned char ld[4];
} le32_t;

typedef struct {
	unsigned char lw[2];
} le16_t;

struct wavriff {
	char magic[4];
	le32_t size;
	char type[4];
};

struct wavchunk {
	char id[4];
	le32_t size;
};

struct wavfmt {
	le16_t fmt;
	le16_t nch;
	le32_t rate;
	le32_t byterate;
	le16_t blkalign;
	le16_t bits;
#define WAV_FMT_SIZE		 16
#define WAV_FMT_SIZE2		(16 + 2)
#define WAV_FMT_EXT_SIZE	(16 + 24)
	le16_t extsize;
	le16_t valbits;
	le32_t chanmask;
	le16_t extfmt;
	char guid[14];
};

struct wavhdr {
	struct wavriff riff;		/* 00..11 */
	struct wavchunk fmt_hdr;	/* 12..20 */
	struct wavfmt fmt;
	struct wavchunk data_hdr;
};

char wav_id_riff[4] = { 'R', 'I', 'F', 'F' };
char wav_id_wave[4] = { 'W', 'A', 'V', 'E' };
char wav_id_data[4] = { 'd', 'a', 't', 'a' };
char wav_id_fmt[4] = { 'f', 'm', 't', ' ' };
char wav_guid[14] = {
	0x00, 0x00, 0x00, 0x00,
	0x10, 0x00, 0x80, 0x00,
	0x00, 0xAA, 0x00, 0x38,
	0x9B, 0x71
};

short wav_ulawmap[256] = {
	-32124, -31100, -30076, -29052, -28028, -27004, -25980, -24956,
	-23932, -22908, -21884, -20860, -19836, -18812, -17788, -16764,
	-15996, -15484, -14972, -14460, -13948, -13436, -12924, -12412,
	-11900, -11388, -10876, -10364,  -9852,  -9340,  -8828,  -8316,
	 -7932,  -7676,  -7420,  -7164,  -6908,  -6652,  -6396,  -6140,
	 -5884,  -5628,  -5372,  -5116,  -4860,  -4604,  -4348,  -4092,
	 -3900,  -3772,  -3644,  -3516,  -3388,  -3260,  -3132,  -3004,
	 -2876,  -2748,  -2620,  -2492,  -2364,  -2236,  -2108,  -1980,
	 -1884,  -1820,  -1756,  -1692,  -1628,  -1564,  -1500,  -1436,
	 -1372,  -1308,  -1244,  -1180,  -1116,  -1052,   -988,   -924,
	  -876,   -844,   -812,   -780,   -748,   -716,   -684,   -652,
	  -620,   -588,   -556,   -524,   -492,   -460,   -428,   -396,
	  -372,   -356,   -340,   -324,   -308,   -292,   -276,   -260,
	  -244,   -228,   -212,   -196,   -180,   -164,   -148,   -132,
	  -120,   -112,   -104,    -96,    -88,    -80,    -72,    -64,
	   -56,    -48,    -40,    -32,    -24,    -16,     -8,      0,
	 32124,  31100,  30076,  29052,  28028,  27004,  25980,  24956,
	 23932,  22908,  21884,  20860,  19836,  18812,  17788,  16764,
	 15996,  15484,  14972,  14460,  13948,  13436,  12924,  12412,
	 11900,  11388,  10876,  10364,   9852,   9340,   8828,   8316,
	  7932,   7676,   7420,   7164,   6908,   6652,   6396,   6140,
	  5884,   5628,   5372,   5116,   4860,   4604,   4348,   4092,
	  3900,   3772,   3644,   3516,   3388,   3260,   3132,   3004,
	  2876,   2748,   2620,   2492,   2364,   2236,   2108,   1980,
	  1884,   1820,   1756,   1692,   1628,   1564,   1500,   1436,
	  1372,   1308,   1244,   1180,   1116,   1052,    988,    924,
	   876,    844,    812,    780,    748,    716,    684,    652,
	   620,    588,    556,    524,    492,    460,    428,    396,
	   372,    356,    340,    324,    308,    292,    276,    260,
	   244,    228,    212,    196,    180,    164,    148,    132,
	   120,    112,    104,     96,     88,     80,     72,     64,
	    56,     48,     40,     32,     24,     16,      8,      0
};

short wav_alawmap[256] = {
	 -5504,  -5248,  -6016,  -5760,  -4480,  -4224,  -4992,  -4736,
	 -7552,  -7296,  -8064,  -7808,  -6528,  -6272,  -7040,  -6784,
	 -2752,  -2624,  -3008,  -2880,  -2240,  -2112,  -2496,  -2368,
	 -3776,  -3648,  -4032,  -3904,  -3264,  -3136,  -3520,  -3392,
	-22016, -20992, -24064, -23040, -17920, -16896, -19968, -18944,
	-30208, -29184, -32256, -31232, -26112, -25088, -28160, -27136,
	-11008, -10496, -12032, -11520,  -8960,  -8448,  -9984,  -9472,
	-15104, -14592, -16128, -15616, -13056, -12544, -14080, -13568,
	  -344,   -328,   -376,   -360,   -280,   -264,   -312,   -296,
	  -472,   -456,   -504,   -488,   -408,   -392,   -440,   -424,
	   -88,    -72,   -120,   -104,    -24,     -8,    -56,    -40,
	  -216,   -200,   -248,   -232,   -152,   -136,   -184,   -168,
	 -1376,  -1312,  -1504,  -1440,  -1120,  -1056,  -1248,  -1184,
	 -1888,  -1824,  -2016,  -1952,  -1632,  -1568,  -1760,  -1696,
	  -688,   -656,   -752,   -720,   -560,   -528,   -624,   -592,
	  -944,   -912,  -1008,   -976,   -816,   -784,   -880,   -848,
	  5504,   5248,   6016,   5760,   4480,   4224,   4992,   4736,
	  7552,   7296,   8064,   7808,   6528,   6272,   7040,   6784,
	  2752,   2624,   3008,   2880,   2240,   2112,   2496,   2368,
	  3776,   3648,   4032,   3904,   3264,   3136,   3520,   3392,
	 22016,  20992,  24064,  23040,  17920,  16896,  19968,  18944,
	 30208,  29184,  32256,  31232,  26112,  25088,  28160,  27136,
	 11008,  10496,  12032,  11520,   8960,   8448,   9984,   9472,
	 15104,  14592,  16128,  15616,  13056,  12544,  14080,  13568,
	   344,    328,    376,    360,    280,    264,    312,    296,
	   472,    456,    504,    488,    408,    392,    440,    424,
	    88,     72,    120,    104,     24,      8,     56,     40,
	   216,    200,    248,    232,    152,    136,    184,    168,
	  1376,   1312,   1504,   1440,   1120,   1056,   1248,   1184,
	  1888,   1824,   2016,   1952,   1632,   1568,   1760,   1696,
	   688,    656,    752,    720,    560,    528,    624,    592,
	   944,    912,   1008,    976,    816,    784,    880,    848
};

static inline unsigned int
le16_get(le16_t *p)
{
	return p->lw[0] | p->lw[1] << 8;
}

static inline void
le16_set(le16_t *p, unsigned int v)
{
	p->lw[0] = v;
	p->lw[1] = v >> 8;
}

static inline unsigned int
le32_get(le32_t *p)
{
	return p->ld[0] |
	       p->ld[1] << 8 |
	       p->ld[2] << 16 |
	       p->ld[3] << 24;
}

static inline void
le32_set(le32_t *p, unsigned int v)
{
	p->ld[0] = v;
	p->ld[1] = v >> 8;
	p->ld[2] = v >> 16;
	p->ld[3] = v >> 24;
}

static int
wav_readfmt(struct wav *w, unsigned int csize)
{
	struct wavfmt fmt;
	unsigned int nch, rate, bits, bps, enc;

	if (csize < WAV_FMT_SIZE) {
		log_putu(csize);
		log_puts(": bugus format chunk size\n");
		return 0;
	}
	if (csize > WAV_FMT_EXT_SIZE)
		csize = WAV_FMT_EXT_SIZE;
	if (read(w->fd, &fmt, csize) != csize) {
		log_puts("failed to read .wav format chun\n");
		return 0;
	}
	enc = le16_get(&fmt.fmt);
	bits = le16_get(&fmt.bits);
	if (enc == WAV_ENC_EXT) {
		if (csize != WAV_FMT_EXT_SIZE) {
			log_puts("missing extended format chunk in .wav file\n");
			return 0;
		}
		if (memcmp(fmt.guid, wav_guid, sizeof(wav_guid)) != 0) {
			log_puts("unknown format (GUID) in .wav file\n");
			return 0;
		}
		bps = (bits + 7) / 8;
		bits = le16_get(&fmt.valbits);
		enc = le16_get(&fmt.extfmt);
	} else
		bps = (bits + 7) / 8;
	switch (enc) {
	case WAV_ENC_PCM:
		w->map = NULL;
		break;
	case WAV_ENC_ALAW:
		w->map = wav_alawmap;
		break;
	case WAV_ENC_ULAW:
		w->map = wav_ulawmap;
		break;
	default:
		log_putu(enc);
		log_puts(": unsupported encoding in .wav file\n");
		return 0;
	}
	nch = le16_get(&fmt.nch);
	if (nch == 0) {
		log_puts("zero number of channels in .wav file\n");
		return 0;
	}
	rate = le32_get(&fmt.rate);
	if (rate < RATE_MIN || rate > RATE_MAX) {
		log_putu(rate);
		log_puts(": bad sample rate in .wav file\n");
		return 0;
	}
	if (bits < BITS_MIN || bits > BITS_MAX) {
		log_putu(bits);
		log_puts(": bad number of bits\n");
		return 0;
	}
	if (bits > bps * 8) {
		log_puts("bits larger than bytes-per-sample\n");
		return 0;
	}
	if (enc == WAV_ENC_PCM) {
		w->par.bps = bps;
		w->par.bits = bits;
		w->par.le = 1;
		w->par.sig = (bits <= 8) ? 0 : 1;	/* ask microsoft why... */
		w->par.msb = 1;
	} else {
		if (bits != 8) {
			log_puts("mulaw/alaw encoding not 8-bit\n");
			return 0;
		}
		w->par.bits = ADATA_BITS;
		w->par.bps = sizeof(adata_t);
		w->par.le = ADATA_LE;
		w->par.sig = 1;
		w->par.msb = 0;
	}
	w->nch = nch;
	w->rate = rate;
	return 1;
}

static int
wav_readhdr(struct wav *w)
{
	struct wavriff riff;
	struct wavchunk chunk;
	unsigned int csize, rsize, pos = 0;
	int fmt_done = 0;

	if (lseek(w->fd, 0, SEEK_SET) < 0) {
		log_puts("failed to seek to beginning of .wav file\n");
		return 0;
	}
	if (read(w->fd, &riff, sizeof(riff)) != sizeof(riff)) {
		log_puts("failed to read .wav file riff header\n");
		return 0;
	}
	if (memcmp(&riff.magic, &wav_id_riff, 4) != 0 ||
	    memcmp(&riff.type, &wav_id_wave, 4)) {
		log_puts("not a .wav file\n");
		return 0;
	}
	rsize = le32_get(&riff.size);
	for (;;) {
		if (pos + sizeof(struct wavchunk) > rsize) {
			log_puts("missing data chunk in .wav file\n");
			return 0;
		}
		if (read(w->fd, &chunk, sizeof(chunk)) != sizeof(chunk)) {
			log_puts("failed to read .wav chunk header\n");
			return 0;
		}
		csize = le32_get(&chunk.size);
		if (memcmp(chunk.id, wav_id_fmt, 4) == 0) {
			if (!wav_readfmt(w, csize))
				return 0;
			fmt_done = 1;
		} else if (memcmp(chunk.id, wav_id_data, 4) == 0) {
			w->startpos = pos + sizeof(riff) + sizeof(chunk);
			w->endpos = w->startpos + csize;
			break;
		} else {
#ifdef DEBUG
			if (log_level >= 2)
				log_puts("skipped unknown .wav file chunk\n");
#endif
		}

		/*
		 * next chunk
		 */
		pos += sizeof(struct wavchunk) + csize;
		if (lseek(w->fd, sizeof(riff) + pos, SEEK_SET) < 0) {
			log_puts("filed to seek to chunk in .wav file\n");
			return 0;
		}
	}
	if (!fmt_done) {
		log_puts("missing format chunk in .wav file\n");
		return 0;
	}
	return 1;
}

/*
 * Write header and seek to start position
 */
static int
wav_writehdr(struct wav *w)
{
	struct wavhdr hdr;

	memset(&hdr, 0, sizeof(struct wavhdr));
	memcpy(hdr.riff.magic, wav_id_riff, 4);
	memcpy(hdr.riff.type, wav_id_wave, 4);
	le32_set(&hdr.riff.size, w->endpos - sizeof(hdr.riff));
	memcpy(hdr.fmt_hdr.id, wav_id_fmt, 4);
	le32_set(&hdr.fmt_hdr.size, sizeof(hdr.fmt));
	le16_set(&hdr.fmt.fmt, 1);
	le16_set(&hdr.fmt.nch, w->nch);
	le32_set(&hdr.fmt.rate, w->rate);
	le32_set(&hdr.fmt.byterate, w->rate * w->par.bps * w->nch);
	le16_set(&hdr.fmt.blkalign, w->par.bps * w->nch);
	le16_set(&hdr.fmt.bits, w->par.bits);
	memcpy(hdr.data_hdr.id, wav_id_data, 4);
	le32_set(&hdr.data_hdr.size, w->endpos - w->startpos);

	if (lseek(w->fd, 0, SEEK_SET) < 0) {
		log_puts("failed to seek back to .wav file header\n");
		return 0;
	}
	if (write(w->fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		log_puts("failed to write .wav file header\n");
		return 0;
	}
	w->curpos = w->startpos;
	return 1;
}

/*
 * convert ``count'' samples using the given char->short map
 */
static void
wav_conv(unsigned char *data, unsigned int count, short *map)
{
	unsigned int i;
	unsigned char *iptr;
	adata_t *optr;

	iptr = data + count;
	optr = (adata_t *)data + count;
	for (i = count; i > 0; i--) {
		--optr;
		--iptr;
		*optr = (adata_t)(map[*iptr]) << (ADATA_BITS - 16);
	}
}

size_t
wav_read(struct wav *w, void *data, size_t count)
{
	off_t maxread;
	ssize_t n;

	if (w->map)
		count /= 2;
	if (w->endpos >= 0) {
		maxread = w->endpos - w->curpos;
		if (maxread == 0) {
#ifdef DEBUG
			if (log_level >= 3) {
				log_puts(w->path);
				log_puts(": end reached\n");
			}
#endif
			return 0;
		}
		if (count > maxread)
			count = maxread;
	}
	n = read(w->fd, data, count);
	if (n < 0) {
		log_puts(w->path);
		log_puts(": couldn't read\n");
		return 0;
	}
	w->curpos += n;
	if (w->map) {
		wav_conv(data, n, w->map);
		n *= 2;
	}
	return n;
}

size_t
wav_write(struct wav *w, void *data, size_t count)
{
	off_t maxwrite;
	int n;

	if (w->maxpos >= 0) {
		maxwrite = w->maxpos - w->curpos;
		if (maxwrite == 0) {
#ifdef DEBUG
			if (log_level >= 3) {
				log_puts(w->path);
				log_puts(": max file size reached\n");
			}
#endif
			return 0;
		}
		if (count > maxwrite)
			count = maxwrite;
	}
	n = write(w->fd, data, count);
	if (n < 0) {
		log_puts(w->path);
		log_puts(": couldn't write\n");
		return 0;
	}
	w->curpos += n;
	if (w->endpos < w->curpos)
		w->endpos = w->curpos;
	return n;
}

int
wav_seek(struct wav *w, off_t pos)
{
	if (w->map)
		pos /= 2;
	pos += w->startpos;
	if (w->endpos >= 0 && pos > w->endpos) {
		log_puts(w->path);
		log_puts(": attempt to seek ouside file boundaries\n");
		return 0;
	}

	/*
	 * seek only if needed to avoid errors with pipes & sockets
	 */
	if (pos != w->curpos) {
		if (lseek(w->fd, pos, SEEK_SET) < 0) {
			log_puts(w->path);
			log_puts(": couldn't seek\n");
			return 0;
		}
		w->curpos = pos;
	}
	return 1;
}

void
wav_close(struct wav *w)
{
	if (w->flags & WAV_FWRITE) {
		if (w->hdr == HDR_WAV)
			wav_writehdr(w);
	}
	close(w->fd);
}

int
wav_open(struct wav *w, char *path, int hdr, int flags,
    struct aparams *par, int rate, int nch)
{
	char *ext;
	struct wavhdr dummy;

	w->par = *par;
	w->rate = rate;
	w->nch = nch;
	w->flags = flags;
	w->hdr = hdr;
	if (hdr == HDR_AUTO) {
		w->hdr = HDR_RAW;
		ext = strrchr(path, '.');
		if (ext != NULL) {
			ext++;
			if (strcasecmp(ext, "wav") == 0)
				w->hdr = HDR_WAV;
		}
	}
	if (w->flags == WAV_FREAD) {
		if (strcmp(path, "-") == 0) {
			w->path = "stdin";
			w->fd = STDIN_FILENO;
		} else {
			w->path = path;
			w->fd = open(w->path, O_RDONLY, 0);
			if (w->fd < 0) {
				log_puts(w->path);
				log_puts(": failed to open for reading\n");
				return 0;
			}
		}
		if (w->hdr == HDR_WAV) {
			if (!wav_readhdr(w))
				goto bad_close;
		} else {
			w->startpos = 0;
			w->endpos = -1; /* read until EOF */
			w->map = NULL;
		}
		w->curpos = w->startpos;
	} else if (flags == WAV_FWRITE) {
		if (strcmp(path, "-") == 0) {
			w->path = "stdout";
			w->fd = STDOUT_FILENO;
		} else {
			w->path = path;
			w->fd = open(w->path, O_WRONLY | O_TRUNC | O_CREAT, 0666);
			if (w->fd < 0) {
				log_puts(w->path);
				log_puts(": failed to create file\n");
				return 0;
			}
		}
		if (w->hdr == HDR_WAV) {
			w->par.bps = (w->par.bits + 7) >> 3;
			if (w->par.bits > 8) {
				w->par.le = 1;
				w->par.sig = 1;
			} else
				w->par.sig = 0;
			if (w->par.bits & 7)
				w->par.msb = 1;
			w->endpos = w->startpos = sizeof(struct wavhdr);			
			w->maxpos = WAV_MAXPOS;
			memset(&dummy, 0xd0, sizeof(struct wavhdr));
			if (write(w->fd, &dummy, sizeof(struct wavhdr)) < 0) {
				log_puts(w->path);
				log_puts(": failed reserve space for .wav header\n");
				goto bad_close;
			}
		} else {
			w->endpos = w->startpos = 0;
			w->maxpos = -1;
		}
		w->curpos = w->startpos;
	} else {
#ifdef DEBUG
		log_puts("wav_open: wrong flags\n");
		panic();
#endif
	}
	return 1;
bad_close:
	close(w->fd);
	return 0;
}
