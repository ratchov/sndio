/*
 * Copyright (c) 2003-2010 Alexandre Ratchov <alex@caoua.org>
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
#include <sndio.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "smf.h"

#define SMF_SYSEX		0xf0
#define SMF_RAW			0xf7
#define SMF_META		0xff
#define SMF_META_END		0x2f
#define SMF_META_TEMPO		0x51
#define SMF_STATUS		0x80
#define SMF_IS_VOICE(c)		((c) < 0xf0)

struct smf_chunk {
	unsigned char *pos, *end;
};

struct smf_track {
	struct smf_track *next;
	struct smf_chunk chunk;
	unsigned int status;
	unsigned int delta;
};

struct smf {
	struct smf_track *track_list;
	struct smf_chunk root;
	int (*cb)(void *, unsigned int);
	void *arg;
	unsigned int tempo;		/* microsecs per quarter note */
	unsigned int div;		/* ticks per quarter note */
	unsigned char data[];
};

unsigned char smf_id_hdr[4] = {'M', 'T', 'h', 'd'};
unsigned char smf_id_trk[4] = {'M', 'T', 'r', 'k'};
unsigned int smf_voice_len[] = {2, 2, 2, 2, 1, 1, 2, 0};

/*
 * Read the number stored in the next "nbytes" bytes.
 */
static int
smf_getnum(struct smf_chunk *f, int nbytes, unsigned int *rval)
{
	unsigned int val, shift;

	if (f->end - f->pos < nbytes) {
		fprintf(stderr, "failed to read number\n");
		return 0;
	}
	val = 0;
	shift = 8 * nbytes;
	while (shift > 0) {
		shift -= 8;
		val += *f->pos++ << shift;
	}
	*rval = val;
	return 1;
}

/*
 * Read the variable lenght number stored in the next bytes.
 */
static int
smf_getvar(struct smf_chunk *f, unsigned int *rval)
{
	unsigned int c, bytes, val;

	val = 0;
	bytes = 0;
	while (1) {
		if (f->pos == f->end || bytes == 4) {
			fprintf(stderr, "failed to read var num\n");
			return 0;
		}
		c = *f->pos++;
		val = (val << 7) | (c & 0x7f);
		if ((c & 0x80) == 0)
			break;
		bytes++;
	}
	*rval = val;
	return 1;
}

/*
 * Read next chunk header and check if it's of the expected type.
 */
static int
smf_getchunk(struct smf_chunk *f, unsigned char *id, struct smf_chunk *result)
{
	unsigned int size;

	if (f->end - f->pos < 4) {
		fprintf(stderr, "chunk id expected\n");
		return 0;
	}
	if (memcmp(f->pos, id, 4) != 0) {
		fprintf(stderr, "bad chunk id\n");
		return 0;
	}
	f->pos += 4;
	if (!smf_getnum(f, 4, &size))
		return 0;
	result->pos = f->pos;
	result->end = f->pos + size;
	f->pos += size;
	return 1;
}

/*
 * Send next "len" bytes of MIDI data to the device.
 */
static int
smf_sendraw(struct smf *f, struct smf_track *t, unsigned int len)
{
	if (t->chunk.end - t->chunk.pos < len) {
		fprintf(stderr, "data to send out of file boundaries\n");
		return 0;
	}
	while (len-- > 0) {
		if (!f->cb(f->arg, *t->chunk.pos++))
			return 0;
	}
	return 1;
}

/*
 * Read track chunk and prepare to start playback.
 */
static struct smf_track *
smf_gettrack(struct smf_chunk *f)
{
	struct smf_track *t;

	t = malloc(sizeof(struct smf_track));
	if (t == NULL)
		return NULL;
	if (!smf_getchunk(f, smf_id_trk, &t->chunk))
		goto bad_free;
	if (t->chunk.pos != t->chunk.end) {
		if (!smf_getvar(&t->chunk, &t->delta))
			goto bad_free;
	}
	t->status = 0;
	return t;
bad_free:
	free(t);
	return NULL;
}

/*
 * Play all events at the current time position (if any) and update
 * t->delta (the number of ticks before the next event).
 */
static int
smf_play_track(struct smf *s, struct smf_track *t)
{
	unsigned int c, len;

	if (t->delta > 0)
		return 1;
	for (;;) {
		if (!smf_getnum(&t->chunk, 1, &c))
			return 0;
		if (c == SMF_META) {
			if (!smf_getnum(&t->chunk, 1, &c))
				return 0;
			if (!smf_getvar(&t->chunk, &len))
				return 0;
			switch (c) {
			case SMF_META_END:
				t->chunk.pos = t->chunk.end;
				break;
			case SMF_META_TEMPO:
				if (!smf_getnum(&t->chunk, 3, &s->tempo))
					return 0;
				break;
			default:
				if (t->chunk.end - t->chunk.pos < len) {
					fprintf(stderr, "can't skip\n");
					return 0;
				}
				t->chunk.pos += len;
			}
			t->status = 0;
		} else if (c == SMF_RAW) {
			if (!smf_getvar(&t->chunk, &len))
				return 0;
			if (!smf_sendraw(s, t, len))
				return 0;
			t->status = 0;
		} else if (c == SMF_SYSEX) {
			if (!smf_getvar(&t->chunk, &len))
				return 0;
			if (!s->cb(s->arg, 0xf0) || !smf_sendraw(s, t, len))
				return 0;
			t->status = 0;
		} else if (SMF_IS_VOICE(c)) {
			if (c & SMF_STATUS) {
				t->status = c;
				if (!smf_getnum(&t->chunk, 1, &c))
					return 0;
			}
			if (t->status == 0) {
				fprintf(stderr, "bad status byte %02x\n", c);
				return 0;
			}
			if (!s->cb(s->arg, t->status) || !s->cb(s->arg, c))
				return 0;
			if (smf_voice_len[((t->status) >> 4) & 0x07] == 2) {
				if (!smf_getnum(&t->chunk, 1, &c) ||
				    !s->cb(s->arg, c))
					return 0;
			}
		} else {
			fprintf(stderr, "bad record type: %02x\n", c);
			return 0;
		}
		if (t->chunk.pos == t->chunk.end)
			break;
		if (!smf_getvar(&t->chunk, &t->delta))
			return 0;
		if (t->delta > 0)
			break;
	}
	return 1;
}

/*
 * Open MIDI file, load it in memory and prepare to start playback.
 */
struct smf *
smf_open(char *path, int (*cb)(void *, unsigned int), void *arg)
{
	struct smf *f;
	struct smf_chunk hdr;
	struct smf_track *t, **endp;
	off_t size;
	unsigned int format, ntrks;
	int fd;

	fd = open(path, O_RDONLY, 0);
	if (fd < 0) {
		perror("path");
		return NULL;
	}
	size = lseek(fd, 0, SEEK_END);
	if (size < 0) {
		perror("seek");
		goto bad_close;
	}
	f = malloc(size + offsetof(struct smf, data));
	if (f == NULL) {
		perror("malloc");
		goto bad_close;
	}
	if (pread(fd, f->data, size, 0) != size) {
		fprintf(stderr, "%s: couldn't read file\n", path);
		goto bad_free;
	}
	f->root.pos = f->data;
	f->root.end = f->data + size;
	f->track_list = NULL;
	f->cb = cb;
	f->arg = arg;

	/*
	 * parse header
	 */
	if (!smf_getchunk(&f->root, smf_id_hdr, &hdr))
		goto bad_free;
	if (!smf_getnum(&hdr, 2, &format))
		goto bad_free;
	if (!smf_getnum(&hdr, 2, &ntrks))
		goto bad_free;
	if (!smf_getnum(&hdr, 2, &f->div))
		goto bad_free;
	if (format != 1 && format != 0) {
		fprintf(stderr, "only file format 0 or 1 are supported\n");
		goto bad_free;
	}
	if ((f->div & 0x8000) != 0) {
		fprintf(stderr, "smpte timecode is not supported\n");
		goto bad_free;
	}
	f->tempo = 1000000 * f->div / (120 * 4);

	/*
	 * parse tracks
	 */
	endp = &f->track_list;
	f->track_list = NULL;
	while (ntrks > 0) {
		t = smf_gettrack(&f->root);
		if (t == NULL)
			goto bad_free_tracks;
		t->next = NULL;
		*endp = t;
		endp = &t->next;
		ntrks--;
	}
	close(fd);
	return f;
bad_free_tracks:
	while (f->track_list) {
		t = f->track_list;
		f->track_list = t->next;
		free(t);
	}
bad_free:
	free(f);
bad_close:
	close(fd);
	return 0;
}

/*
 * Free all resources.
 */
void
smf_close(struct smf *f)
{
	struct smf_track *t;

	while (f->track_list) {
		t = f->track_list;
		f->track_list = t->next;
		free(t);
	}
	free(f);
}

/*
 * Play all events at the current time position and advance to the
 * next position. Return the number of nanoseconds until the next
 * position.
 */
int
smf_play(struct smf *f, long long *rdelta_nsec)
{
	struct smf_track *t;
	unsigned int delta;

	delta = ~0U;
	for (t = f->track_list; t != NULL; t = t->next) {
		if (t->chunk.pos == t->chunk.end)
			continue;
		if (!smf_play_track(f, t))
			return 0;
		if (t->chunk.pos == t->chunk.end)
			continue;
		if (t->delta < delta)
			delta = t->delta;
	}

	if (delta == ~0U) {
		*rdelta_nsec = 0;
		return 1;
	}

	for (t = f->track_list; t != NULL; t = t->next) {
		if (t->chunk.pos == t->chunk.end)
			continue;
		t->delta -= delta;
	}

	*rdelta_nsec = 1000LL * delta * f->tempo / f->div;
	return 1;
}
