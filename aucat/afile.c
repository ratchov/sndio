/*
 * Copyright (c) 2008-2014 Alexandre Ratchov <alex@caoua.org>
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
#include "afile.h"
#include "utils.h"

/*
 * Max size of a .wav file, format design limitation.
 */
#define WAV_MAXPOS	(0x7fffffff)

/*
 * Encoding IDs used in .wav headers.
 */
#define WAV_FMT_PCM	1
#define WAV_FMT_FLOAT	3
#define WAV_FMT_ALAW	6
#define WAV_FMT_ULAW	7
#define WAV_FMT_EXT	0xfffe

#define AU_FMT_PCM8	2
#define AU_FMT_PCM16	3
#define AU_FMT_PCM24	4
#define AU_FMT_PCM32	5
#define AU_FMT_FLOAT	6
#define AU_FMT_ALAW	0x1b
#define AU_FMT_ULAW	1

typedef struct {
	unsigned char ld[4];
} le32_t;

typedef struct {
	unsigned char lw[2];
} le16_t;

typedef struct {
	unsigned char bd[4];
} be32_t;

typedef struct {
	unsigned char bw[2];
} be16_t;

struct wav_riff {
	char magic[4];
	le32_t size;
	char type[4];
};

struct wav_chunk {
	char id[4];
	le32_t size;
};

struct wav_fmt {
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

struct wav_hdr {
	struct wav_riff riff;		/* 00..11 */
	struct wav_chunk fmt_hdr;	/* 12..20 */
	struct wav_fmt fmt;
	struct wav_chunk data_hdr;
};

struct aiff_form {
	char magic[4];
	be32_t size;
	char type[4];
};

struct aiff_chunk {
	char id[4];
	be32_t size;
};

struct aiff_comm {
	struct aiff_commbase {
		be16_t nch;
		be32_t nfr;
		be16_t bits;
		/* rate in 80-bit floating point */
		be16_t rate_ex;
		be32_t rate_hi;
		be32_t rate_lo;
	} base;
	char comp_id[4];
	/* followed by stuff we don't care about */
};

struct aiff_data {
	be32_t offs;
	be32_t blksz;
};

struct aiff_hdr {
	struct aiff_form form;
	struct aiff_chunk comm_hdr;
	struct aiff_commbase comm;
	struct aiff_chunk data_hdr;
	struct aiff_data data;
};

struct au_hdr {
	char id[4];
	be32_t offs;
	be32_t size;
	be32_t fmt;
	be32_t rate;
	be32_t nch;
};

char wav_id_riff[4] = {'R', 'I', 'F', 'F'};
char wav_id_wave[4] = {'W', 'A', 'V', 'E'};
char wav_id_data[4] = {'d', 'a', 't', 'a'};
char wav_id_fmt[4] = {'f', 'm', 't', ' '};
char wav_guid[14] = {
	0x00, 0x00, 0x00, 0x00,
	0x10, 0x00, 0x80, 0x00,
	0x00, 0xAA, 0x00, 0x38,
	0x9B, 0x71
};

char aiff_id_form[4] = {'F', 'O', 'R', 'M'};
char aiff_id_aiff[4] = {'A', 'I', 'F', 'F'};
char aiff_id_aifc[4] = {'A', 'I', 'F', 'C'};
char aiff_id_data[4] = {'S', 'S', 'N', 'D'};
char aiff_id_comm[4] = {'C', 'O', 'M', 'M'};
char aiff_id_none[4] = {'N', 'O', 'N', 'E'};
char aiff_id_fl32[4] = {'f', 'l', '3', '2'};
char aiff_id_ulaw[4] = {'u', 'l', 'a', 'w'};
char aiff_id_alaw[4] = {'a', 'l', 'a', 'w'};

char au_id[4] = {'.', 's', 'n', 'd'};

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

static inline unsigned int
be16_get(be16_t *p)
{
	return p->bw[1] | p->bw[0] << 8;
}

static inline void
be16_set(be16_t *p, unsigned int v)
{
	p->bw[1] = v;
	p->bw[0] = v >> 8;
}

static inline unsigned int
be32_get(be32_t *p)
{
	return p->bd[3] |
	       p->bd[2] << 8 |
	       p->bd[1] << 16 |
	       p->bd[0] << 24;
}

static inline void
be32_set(be32_t *p, unsigned int v)
{
	p->bd[3] = v;
	p->bd[2] = v >> 8;
	p->bd[1] = v >> 16;
	p->bd[0] = v >> 24;
}

static int
afile_wav_readfmt(struct afile *f, unsigned int csize)
{
	struct wav_fmt fmt;
	unsigned int nch, rate, bits, bps, enc;

	if (csize < WAV_FMT_SIZE) {
		log_putu(csize);
		log_puts(": bugus format chunk size\n");
		return 0;
	}
	if (csize > WAV_FMT_EXT_SIZE)
		csize = WAV_FMT_EXT_SIZE;
	if (read(f->fd, &fmt, csize) != csize) {
		log_puts("failed to read .wav format chun\n");
		return 0;
	}
	enc = le16_get(&fmt.fmt);
	bits = le16_get(&fmt.bits);
	if (enc == WAV_FMT_EXT) {
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
	switch (enc) {
	case WAV_FMT_PCM:
		f->enc = ENC_PCM;
		f->par.bps = bps;
		f->par.bits = bits;
		f->par.le = 1;
		f->par.sig = (bits <= 8) ? 0 : 1;	/* ask microsoft ... */
		f->par.msb = 1;
		break;
	case WAV_FMT_ALAW:
	case WAV_FMT_ULAW:
		f->enc = (enc == WAV_FMT_ULAW) ? ENC_ULAW : ENC_ALAW;
		if (bits != 8) {
			log_puts("mulaw/alaw encoding not 8-bit\n");
			return 0;
		}
		f->par.bits = 8;
		f->par.bps = 1;
		f->par.le = ADATA_LE;
		f->par.sig = 0;
		f->par.msb = 0;
		break;
	case WAV_FMT_FLOAT:
		f->enc = ENC_FLOAT;
		if (bits != 32) {
			log_puts("only 32-bit float supported\n");
			return 0;
		}
		f->par.bits = 32;
		f->par.bps = 4;
		f->par.le = 1;
		f->par.sig = 0;
		f->par.msb = 0;
		break;
	default:
		log_putu(enc);
		log_puts(": unsupported encoding in .wav file\n");
		return 0;
	}
	f->nch = nch;
	f->rate = rate;
	return 1;
}

static int
afile_wav_readhdr(struct afile *f)
{
	struct wav_riff riff;
	struct wav_chunk chunk;
	unsigned int csize, rsize, pos = 0;
	int fmt_done = 0;

	if (lseek(f->fd, 0, SEEK_SET) < 0) {
		log_puts("failed to seek to beginning of .wav file\n");
		return 0;
	}
	if (read(f->fd, &riff, sizeof(riff)) != sizeof(riff)) {
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
		if (pos + sizeof(struct wav_chunk) > rsize) {
			log_puts("missing data chunk in .wav file\n");
			return 0;
		}
		if (read(f->fd, &chunk, sizeof(chunk)) != sizeof(chunk)) {
			log_puts("failed to read .wav chunk header\n");
			return 0;
		}
		csize = le32_get(&chunk.size);
		if (memcmp(chunk.id, wav_id_fmt, 4) == 0) {
			if (!afile_wav_readfmt(f, csize))
				return 0;
			fmt_done = 1;
		} else if (memcmp(chunk.id, wav_id_data, 4) == 0) {
			f->startpos = pos + sizeof(riff) + sizeof(chunk);
			f->endpos = f->startpos + csize;
			break;
		} else {
#ifdef DEBUG
			if (log_level >= 2) {
				log_puts(f->path);
				log_puts(": skipped unknown chunk\n");
			}
#endif
		}

		/*
		 * next chunk
		 */
		pos += sizeof(struct wav_chunk) + csize;
		if (lseek(f->fd, sizeof(riff) + pos, SEEK_SET) < 0) {
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
afile_wav_writehdr(struct afile *f)
{
	struct wav_hdr hdr;

	memset(&hdr, 0, sizeof(struct wav_hdr));
	memcpy(hdr.riff.magic, wav_id_riff, 4);
	memcpy(hdr.riff.type, wav_id_wave, 4);
	le32_set(&hdr.riff.size, f->endpos - sizeof(hdr.riff));
	memcpy(hdr.fmt_hdr.id, wav_id_fmt, 4);
	le32_set(&hdr.fmt_hdr.size, sizeof(hdr.fmt));
	le16_set(&hdr.fmt.fmt, 1);
	le16_set(&hdr.fmt.nch, f->nch);
	le32_set(&hdr.fmt.rate, f->rate);
	le32_set(&hdr.fmt.byterate, f->rate * f->par.bps * f->nch);
	le16_set(&hdr.fmt.blkalign, f->par.bps * f->nch);
	le16_set(&hdr.fmt.bits, f->par.bits);
	memcpy(hdr.data_hdr.id, wav_id_data, 4);
	le32_set(&hdr.data_hdr.size, f->endpos - f->startpos);

	if (lseek(f->fd, 0, SEEK_SET) < 0) {
		log_puts("failed to seek back to .wav file header\n");
		return 0;
	}
	if (write(f->fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		log_puts("failed to write .wav file header\n");
		return 0;
	}
	f->curpos = f->startpos;
	return 1;
}

static int
afile_aiff_readcomm(struct afile *f, unsigned int csize,
    int comp, unsigned int *nfr)
{
	struct aiff_comm comm;
	unsigned int csize_min, nch, rate, bits;
	unsigned int e, m;

	csize_min = comp ?
	    sizeof(struct aiff_comm) : sizeof(struct aiff_commbase);
	if (csize < csize_min) {
		log_putu(csize);
		log_puts(": bugus comm chunk size\n");
		return 0;
	}
	if (read(f->fd, &comm, csize_min) != csize_min) {
		log_puts("failed to read .aiff comm chunk\n");
		return 0;
	}
	nch = be16_get(&comm.base.nch);
	if (nch == 0 || nch > NCHAN_MAX) {
		log_putu(nch);
		log_puts(": unsupported number of channels in .aiff file\n");
		return 0;
	}
	e = be16_get(&comm.base.rate_ex);
	m = be32_get(&comm.base.rate_hi);
	if (e < 0x3fff || e > 0x3fff + 31) {
		log_puts(f->path);
		log_puts(": bad sample rate in .aiff file\n");
		return 0;
	}
	rate = m >> (0x3fff + 31 - e);
	if (rate < RATE_MIN || rate > RATE_MAX) {
		log_putu(rate);
		log_puts(": sample rate out of range in .aiff file\n");
		return 0;
	}
	bits = be16_get(&comm.base.bits);
	if (bits < BITS_MIN || bits > BITS_MAX) {
		log_putu(bits);
		log_puts(": bad number of bits in .aiff file\n");
		return 0;
	}
	if (comp) {
		if (memcmp(comm.comp_id, aiff_id_none, 4) == 0) {
			f->enc = ENC_PCM;
		} else if (memcmp(comm.comp_id, aiff_id_fl32, 4) == 0) {
			f->enc = ENC_FLOAT;
		} else if (memcmp(comm.comp_id, aiff_id_ulaw, 4) == 0) {
			f->enc = ENC_ULAW;
		} else if (memcmp(comm.comp_id, aiff_id_alaw, 4) == 0) {
			f->enc = ENC_ALAW;
		} else {
			log_puts("unsupported encoding of .aiff file\n");
			return 0;
		}
	} else
		f->enc = ENC_PCM;
	switch (f->enc) {
	case ENC_PCM:
		f->par.bps = (bits + 7) / 8;
		f->par.bits = bits;
		f->par.le = 0;
		f->par.sig = 1;
		f->par.msb = 1;
		break;
	case ENC_ALAW:
	case ENC_ULAW:
		if (bits != 8) {
			log_puts("mulaw/alaw encoding not 8-bit\n");
			return 0;
		}
		f->par.bits = 8;
		f->par.bps = 1;
		f->par.le = ADATA_LE;
		f->par.sig = 0;
		f->par.msb = 0;
		break;
	case ENC_FLOAT:
		if (bits != 32) {
			log_puts("only 32-bit float supported\n");
			return 0;
		}
		f->par.bits = 32;
		f->par.bps = 4;
		f->par.le = 0;
		f->par.sig = 0;
		f->par.msb = 0;
	}
	f->nch = nch;
	*nfr = be32_get(&comm.base.nfr);
	return 1;
}

static int
afile_aiff_readdata(struct afile *f, unsigned int csize, unsigned int *roffs)
{
	struct aiff_data data;

	if (csize < sizeof(struct aiff_data)) {
		log_putu(csize);
		log_puts(": bugus comm chunk size\n");
		return 0;
	}
	csize = sizeof(struct aiff_data);
	if (read(f->fd, &data, csize) != csize) {
		log_puts("failed to read .aiff data chunk\n");
		return 0;
	}
	*roffs = csize + be32_get(&data.offs);
	return 1;
}

static int
afile_aiff_readhdr(struct afile *f)
{
	struct aiff_form form;
	struct aiff_chunk chunk;
	unsigned int csize, rsize, nfr = 0, pos = 0, offs;
	int comm_done = 0, comp;

	if (lseek(f->fd, 0, SEEK_SET) < 0) {
		log_puts("failed to seek to beginning of .aiff file\n");
		return 0;
	}
	if (read(f->fd, &form, sizeof(form)) != sizeof(form)) {
		log_puts("failed to read .aiff file form header\n");
		return 0;
	}
	if (memcmp(&form.magic, &aiff_id_form, 4) != 0) {
		log_puts(f->path);
		log_puts(": not an aiff file\n");
		return 0;
	}
	if (memcmp(&form.type, &aiff_id_aiff, 4) == 0) {
		comp = 0;
	} else if (memcmp(&form.type, &aiff_id_aifc, 4) == 0)
		comp = 1;
	else {
		log_puts(f->path);
		log_puts(": unknown aiff file type\n");
		return 0;
	}
	rsize = be32_get(&form.size);
	for (;;) {
		if (pos + sizeof(struct aiff_chunk) > rsize) {
			log_puts("missing data chunk in .aiff file\n");
			return 0;
		}
		if (read(f->fd, &chunk, sizeof(chunk)) != sizeof(chunk)) {
			log_puts("failed to read .aiff chunk header\n");
			return 0;
		}
		csize = be32_get(&chunk.size);
		if (memcmp(chunk.id, aiff_id_comm, 4) == 0) {
			if (!afile_aiff_readcomm(f, csize, comp, &nfr))
				return 0;
			comm_done = 1;
		} else if (memcmp(chunk.id, aiff_id_data, 4) == 0) {
			if (!afile_aiff_readdata(f, csize, &offs))
				return 0;
			f->startpos = sizeof(form) + pos + sizeof(chunk) + offs;
			break;
		} else {
#ifdef DEBUG
			if (log_level >= 2)
				log_puts("skipped unknown .aiff file chunk\n");
#endif
		}

		/*
		 * next chunk
		 */
		pos += sizeof(struct aiff_chunk) + csize;
		if (lseek(f->fd, sizeof(form) + pos, SEEK_SET) < 0) {
			log_puts("filed to seek to chunk in .aiff file\n");
			return 0;
		}
	}
	if (!comm_done) {
		log_puts("missing comm chunk in .wav file\n");
		return 0;
	}
	f->endpos = f->startpos + f->par.bps * f->nch * nfr;
	return 1;
}

/*
 * Write header and seek to start position
 */
static int
afile_aiff_writehdr(struct afile *f)
{
	struct aiff_hdr hdr;
	unsigned int bpf;
	unsigned int e, m;

	/* convert rate to 80-bit float (exponent and fraction part) */
	m = f->rate;
	e = 0x3fff + 31;
	while ((m & 0x80000000) == 0) {
		e--;
		m <<= 1;
	}

	/* bytes per frame */
	bpf = f->nch * f->par.bps;

	memset(&hdr, 0, sizeof(struct aiff_hdr));
	memcpy(hdr.form.magic, aiff_id_form, 4);
	memcpy(hdr.form.type, aiff_id_aiff, 4);
	be32_set(&hdr.form.size, f->endpos - sizeof(hdr.form));

	memcpy(hdr.comm_hdr.id, aiff_id_comm, 4);
	be32_set(&hdr.comm_hdr.size, sizeof(hdr.comm));
	be16_set(&hdr.comm.nch, f->nch);
	be16_set(&hdr.comm.bits, f->par.bits);
	be16_set(&hdr.comm.rate_ex, e);
	be32_set(&hdr.comm.rate_hi, m);
	be32_set(&hdr.comm.rate_lo, 0);
	be32_set(&hdr.comm.nfr, (f->endpos - f->startpos) / bpf);

	memcpy(hdr.data_hdr.id, aiff_id_data, 4);
	be32_set(&hdr.data_hdr.size, f->endpos - f->startpos);
	be32_set(&hdr.data.offs, 0);
	be32_set(&hdr.data.blksz, 0);
	if (lseek(f->fd, 0, SEEK_SET) < 0) {
		log_puts("failed to seek back to .aiff file header\n");
		return 0;
	}
	if (write(f->fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		log_puts("failed to write .aiff file header\n");
		return 0;
	}
	f->curpos = f->startpos;
	return 1;
}

static int
afile_au_readhdr(struct afile *f)
{
	struct au_hdr hdr;
	unsigned int fmt;

	if (lseek(f->fd, 0, SEEK_SET) < 0) {
		log_puts("failed to seek to beginning of .au file\n");
		return 0;
	}
	if (read(f->fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		log_puts("failed to read .au file header\n");
		return 0;
	}
	if (memcmp(&hdr.id, &au_id, 4) != 0) {
		log_puts("not a .au file\n");
		return 0;
	}
	f->startpos = be32_get(&hdr.offs);
	f->endpos = f->startpos + be32_get(&hdr.size);
	fmt = be32_get(&hdr.fmt);
	switch (fmt) {
	case AU_FMT_PCM8:
		f->enc = ENC_PCM;
		f->par.bits = 8;
		break;
	case AU_FMT_PCM16:
		f->enc = ENC_PCM;
		f->par.bits = 16;
		break;
	case AU_FMT_PCM24:
		f->enc = ENC_PCM;
		f->par.bits = 24;
		break;
	case AU_FMT_PCM32:
		f->enc = ENC_PCM;
		f->par.bits = 32;
		break;
	case AU_FMT_ULAW:
		f->enc = ENC_ULAW;
		f->par.bits = 8;
		f->par.bps = 1;
		break;
	case AU_FMT_ALAW:
		f->enc = ENC_ALAW;
		f->par.bits = 8;
		f->par.bps = 1;
		break;
	case AU_FMT_FLOAT:
		f->enc = ENC_FLOAT;
		f->par.bits = 32;
		f->par.bps = 4;
		break;
	default:
		log_putu(fmt);
		log_puts(": unsupported encoding in .au file\n");
		return 0;
	}
	f->par.le = 0;
	f->par.sig = 1;
	f->par.bps = f->par.bits / 8;
	f->par.msb = 0;
	f->rate = be32_get(&hdr.rate);
	if (f->rate < RATE_MIN || f->rate > RATE_MAX) {
		log_putu(f->rate);
		log_puts(": sample rate out of range in .au file\n");
		return 0;
	}
	f->nch = be32_get(&hdr.nch);
	if (f->nch == 0 || f->nch > NCHAN_MAX) {
		log_putu(f->nch);
		log_puts(": unsupported number of channels in .au file\n");
		return 0;
	}
	if (lseek(f->fd, f->startpos, SEEK_SET) < 0) {
		log_puts("failed to seek to .au file payload\n");
		return 0;
	}
	return 1;
}

/*
 * Write header and seek to start position
 */
static int
afile_au_writehdr(struct afile *f)
{
	struct au_hdr hdr;
	unsigned int fmt;

	memset(&hdr, 0, sizeof(struct au_hdr));
	memcpy(hdr.id, au_id, 4);
	be32_set(&hdr.offs, f->startpos);
	be32_set(&hdr.size, f->endpos - f->startpos);
	switch (f->par.bits) {
	case 8:
		fmt = AU_FMT_PCM8;
		break;
	case 16:
		fmt = AU_FMT_PCM16;
		break;
	case 24:
		fmt = AU_FMT_PCM24;
		break;
	case 32:
		fmt = AU_FMT_PCM32;
	default:
		log_puts("afile_au_writehdr: wrong precision\n");
		panic();
		return 0;
	}
	be32_set(&hdr.fmt, fmt);
	be32_set(&hdr.rate, f->rate);
	be32_set(&hdr.nch, f->nch);
	if (lseek(f->fd, 0, SEEK_SET) < 0) {
		log_puts("failed to seek back to .au file header\n");
		return 0;
	}
	if (write(f->fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		log_puts("failed to write .au file header\n");
		return 0;
	}
	f->curpos = f->startpos;
	return 1;
}

size_t
afile_read(struct afile *f, void *data, size_t count)
{
	off_t maxread;
	ssize_t n;

	if (f->endpos >= 0) {
		maxread = f->endpos - f->curpos;
		if (maxread == 0) {
#ifdef DEBUG
			if (log_level >= 3) {
				log_puts(f->path);
				log_puts(": end reached\n");
			}
#endif
			return 0;
		}
		if (count > maxread)
			count = maxread;
	}
	n = read(f->fd, data, count);
	if (n < 0) {
		log_puts(f->path);
		log_puts(": couldn't read\n");
		return 0;
	}
	f->curpos += n;
	return n;
}

size_t
afile_write(struct afile *f, void *data, size_t count)
{
	off_t maxwrite;
	int n;

	if (f->maxpos >= 0) {
		maxwrite = f->maxpos - f->curpos;
		if (maxwrite == 0) {
#ifdef DEBUG
			if (log_level >= 3) {
				log_puts(f->path);
				log_puts(": max file size reached\n");
			}
#endif
			return 0;
		}
		if (count > maxwrite)
			count = maxwrite;
	}
	n = write(f->fd, data, count);
	if (n < 0) {
		log_puts(f->path);
		log_puts(": couldn't write\n");
		return 0;
	}
	f->curpos += n;
	if (f->endpos < f->curpos)
		f->endpos = f->curpos;
	return n;
}

int
afile_seek(struct afile *f, off_t pos)
{
	pos += f->startpos;
	if (f->endpos >= 0 && pos > f->endpos) {
		log_puts(f->path);
		log_puts(": attempt to seek ouside file boundaries\n");
		return 0;
	}

	/*
	 * seek only if needed to avoid errors with pipes & sockets
	 */
	if (pos != f->curpos) {
		if (lseek(f->fd, pos, SEEK_SET) < 0) {
			log_puts(f->path);
			log_puts(": couldn't seek\n");
			return 0;
		}
		f->curpos = pos;
	}
	return 1;
}

void
afile_close(struct afile *f)
{
	if (f->flags & WAV_FWRITE) {
		if (f->hdr == HDR_WAV)
			afile_wav_writehdr(f);
		else if (f->hdr == HDR_AIFF)
			afile_aiff_writehdr(f);
		else if (f->hdr == HDR_AU)
			afile_au_writehdr(f);
	}
	close(f->fd);
}

int
afile_open(struct afile *f, char *path, int hdr, int flags,
    struct aparams *par, int rate, int nch)
{
	char *ext;
	struct wav_hdr dummy;

	f->par = *par;
	f->rate = rate;
	f->nch = nch;
	f->flags = flags;
	f->hdr = hdr;
	if (hdr == HDR_AUTO) {
		f->hdr = HDR_RAW;
		ext = strrchr(path, '.');
		if (ext != NULL) {
			ext++;
			if (strcasecmp(ext, "aif") == 0 ||
			    strcasecmp(ext, "aiff") == 0 ||
			    strcasecmp(ext, "aifc") == 0)
				f->hdr = HDR_AIFF;
			else if (strcasecmp(ext, "au") == 0 ||
			    strcasecmp(ext, "snd") == 0)
				f->hdr = HDR_AU;
			else if (strcasecmp(ext, "wav") == 0)
				f->hdr = HDR_WAV;
		}
	}
	if (f->flags == WAV_FREAD) {
		if (strcmp(path, "-") == 0) {
			f->path = "stdin";
			f->fd = STDIN_FILENO;
		} else {
			f->path = path;
			f->fd = open(f->path, O_RDONLY, 0);
			if (f->fd < 0) {
				log_puts(f->path);
				log_puts(": failed to open for reading\n");
				return 0;
			}
		}
		if (f->hdr == HDR_WAV) {
			if (!afile_wav_readhdr(f))
				goto bad_close;
		} else if (f->hdr == HDR_AIFF) {
			if (!afile_aiff_readhdr(f))
				goto bad_close;
		} else if (f->hdr == HDR_AU) {
			if (!afile_au_readhdr(f))
				goto bad_close;
		} else {
			f->startpos = 0;
			f->endpos = -1; /* read until EOF */
			f->enc = ENC_PCM;
		}
		f->curpos = f->startpos;
	} else if (flags == WAV_FWRITE) {
		if (strcmp(path, "-") == 0) {
			f->path = "stdout";
			f->fd = STDOUT_FILENO;
		} else {
			f->path = path;
			f->fd = open(f->path, O_WRONLY | O_TRUNC | O_CREAT, 0666);
			if (f->fd < 0) {
				log_puts(f->path);
				log_puts(": failed to create file\n");
				return 0;
			}
		}
		if (f->hdr == HDR_WAV) {
			f->par.bps = (f->par.bits + 7) >> 3;
			if (f->par.bits > 8) {
				f->par.le = 1;
				f->par.sig = 1;
			} else
				f->par.sig = 0;
			if (f->par.bits & 7)
				f->par.msb = 1;
			f->endpos = f->startpos = sizeof(struct wav_hdr);
			f->maxpos = WAV_MAXPOS;
			memset(&dummy, 0xd0, sizeof(struct wav_hdr));
			if (write(f->fd, &dummy, sizeof(struct wav_hdr)) < 0) {
				log_puts(f->path);
				log_puts(": couldn't write .wav header\n");
				goto bad_close;
			}
		} else if (f->hdr == HDR_AIFF) {
			f->par.bps = (f->par.bits + 7) >> 3;
			if (f->par.bps > 1)
				f->par.le = 0;
			f->par.sig = 1;
			if (f->par.bits & 7)
				f->par.msb = 1;
			f->endpos = f->startpos = sizeof(struct aiff_hdr);
			f->maxpos = WAV_MAXPOS;
			memset(&dummy, 0xd0, sizeof(struct aiff_hdr));
			if (write(f->fd, &dummy, sizeof(struct aiff_hdr)) < 0) {
				log_puts(f->path);
				log_puts(": couldn't write .aiff header\n");
				goto bad_close;
			}
		} else if (f->hdr == HDR_AU) {
			f->par.bits = (f->par.bits + 7) & ~7;
			f->par.bps = f->par.bits / 8;
			f->par.le = 0;
			f->par.sig = 1;
			f->par.msb = 1;
			f->endpos = f->startpos = sizeof(struct au_hdr);
			f->maxpos = WAV_MAXPOS;
			memset(&dummy, 0xd0, sizeof(struct au_hdr));
			if (write(f->fd, &dummy, sizeof(struct au_hdr)) < 0) {
				log_puts(f->path);
				log_puts(": couldn't write .au header\n");
				goto bad_close;
			}
		} else {
			f->endpos = f->startpos = 0;
			f->maxpos = -1;
		}
		f->curpos = f->startpos;
	} else {
#ifdef DEBUG
		log_puts("afile_open: wrong flags\n");
		panic();
#endif
	}
	return 1;
bad_close:
	close(f->fd);
	return 0;
}
