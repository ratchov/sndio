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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sndio.h>
#include <unistd.h>
#include "sysex.h"

#define MIDI_CMDMASK	0xf0		/* command mask */
#define MIDI_CHANMASK	0x0f		/* channel mask */
#define MIDI_CTL	0xb0		/* controller command */
#define MIDI_CTLVOL	7		/* volume */
#define MIDI_NCHAN	16		/* max channels */
#define MSGMAX		0x100		/* buffer size */

int verbose = 0;
int mst, midx, mlen, mready;		/* midi parser state */
unsigned char mmsg[MSGMAX];		/* resulting midi message */
struct mio_hdl *hdl;			/* handle to sndiod MIDI port */

struct ctl {
	char name[SYSEX_NAMELEN];	/* stream name */
	unsigned vol;			/* current volume */
} ctls[MIDI_NCHAN];
int master = -1;

unsigned char dumpreq[] = {
	SYSEX_START,	
	SYSEX_TYPE_EDU,
	0,
	SYSEX_AUCAT,
	SYSEX_AUCAT_DUMPREQ,
	SYSEX_END
};

void
setvol(int cn, int vol)
{
#define VOLMSGLEN 3
	char msg[VOLMSGLEN];

	msg[0] = MIDI_CTL | cn;
	msg[1] = MIDI_CTLVOL;
	msg[2] = vol;
	if (mio_write(hdl, msg, VOLMSGLEN) != VOLMSGLEN) {
	        fprintf(stderr, "couldn't write message\n");
		exit(1);
	}
	printf("%s -> %u\n", ctls[cn].name, vol);
}

void
setmaster(int vol)
{
	struct sysex msg;

	msg.start = SYSEX_START;
	msg.type = SYSEX_TYPE_RT;
	msg.id0 = SYSEX_CONTROL;
	msg.id1 = SYSEX_MASTER;
	msg.u.master.fine = 0;
	msg.u.master.coarse = vol;
	msg.u.master.end = SYSEX_END;
	if (mio_write(hdl, &msg, SYSEX_SIZE(master)) == 0) {
	        fprintf(stderr, "couldn't write message\n");
		exit(1);
	}
	printf("master -> %u\n", vol);
}

void
onsysex(unsigned char *buf, int len)
{
	int cn, i;
	struct sysex *x = (struct sysex *)buf;

	if (verbose) {
		fprintf(stderr, "sysex: ");
		for (i = 0; i < len; i++)
			fprintf(stderr, " %02x", buf[i]);
		fprintf(stderr, ", len = %u/%zu\n", len, SYSEX_SIZE(slotdesc));
	}

	if (len < SYSEX_SIZE(empty))
		return;
	if (x->type == SYSEX_TYPE_RT &&
	    x->id0 == SYSEX_CONTROL &&
	    x->id1 == SYSEX_MASTER) {
		if (len == SYSEX_SIZE(master))
			master = x->u.master.coarse;
		return;
	}
	if (x->type != SYSEX_TYPE_EDU ||
	    x->id0 != SYSEX_AUCAT)
		return;
	switch(x->id1) {
	case SYSEX_AUCAT_SLOTDESC:
		cn = x->u.slotdesc.chan;
		if (cn >= MIDI_NCHAN) {
			fprintf(stderr, "%u: invalid channel\n", cn);
			exit(1);
		}
		if (memchr(x->u.slotdesc.name, '\0', SYSEX_NAMELEN) == NULL) {
			fprintf(stderr, "%u: invalid channel name\n", cn);
			exit(1);
		}
		strlcpy(ctls[cn].name, x->u.slotdesc.name, SYSEX_NAMELEN);
		ctls[cn].vol = 0;
		break;
	case SYSEX_AUCAT_DUMPEND:
		mready = 1;
		break;
	}
}

void
oncommon(unsigned char *buf, int len)
{
	int cn, vol;

	if ((buf[0] & MIDI_CMDMASK) != MIDI_CTL)
		return;
	if (buf[1] != MIDI_CTLVOL)
		return;
	cn = buf[0] & MIDI_CHANMASK;
	vol = buf[2];
	ctls[cn].vol = vol;
}

void
oninput(unsigned char *buf, int len)
{
	static int voice_len[] = { 3, 3, 3, 3, 2, 2, 3 };
	static int common_len[] = { 0, 2, 3, 2, 0, 0, 1, 1 };
	int c;

	for (; len > 0; len--) {
		c = *buf;
		buf++;

		if (c >= 0xf8) {
			/* clock events not used yet */
		} else if (c >= 0xf0) {
			if (mst == SYSEX_START &&
			    c == SYSEX_END &&
			    midx < MSGMAX) {
				mmsg[midx++] = c;

				onsysex(mmsg, midx);
				continue;
			}
			mmsg[0] = c;
			mlen = common_len[c & 7];
			mst = c;
			midx = 1;
		} else if (c >= 0x80) {
			mmsg[0] = c;
			mlen = voice_len[(c >> 4) & 7];
			mst = c;
			midx = 1;
		} else if (mst) {
			if (midx == MSGMAX)
				continue;		
			if (midx == 0)
				mmsg[midx++] = mst;
			mmsg[midx++] = c;
			if (midx == mlen) {
				oncommon(mmsg, midx);
				midx = 0;
			}
		}
	}
}

void
usage(void)
{
	fprintf(stderr, "usage: sndioctl [-v] [-f port] [expr ...]\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	char *dev = NULL;
	unsigned char buf[MSGMAX], *lhs, *rhs;
	int c, cn, vol, size;

	while ((c = getopt(argc, argv, "f:v")) != -1) {
		switch (c) {
		case 'f':
			dev = optarg;
			break;
		case 'v':
			verbose++;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (dev == NULL)
		dev = getenv("AUDIODEVICE");
	if (dev == NULL)
		dev = "snd/0";

	hdl = mio_open(dev, MIO_OUT | MIO_IN, 0);
	if (hdl == NULL) {
		fprintf(stderr, "%s: couldn't open MIDI device\n", dev);
		exit(1);
	}
	mio_write(hdl, dumpreq, sizeof(dumpreq));
	while (!mready) {
		size = mio_read(hdl, buf, MSGMAX);
		if (size == 0) {
			fprintf(stderr, "%s: read failed\n", dev);
			exit(1);
		}
		oninput(buf, size);
	}
	if (argc == 0) {
		for (cn = 0; cn < MIDI_NCHAN; cn++) {
			if (*ctls[cn].name != '\0') {
				printf("%s=%u\n",
				    ctls[cn].name,
				    ctls[cn].vol);
			}
		}
		if (master >= 0)
			printf("master=%u\n", master);
		return 0;
	}
	for (; argc > 0; argc--, argv++) {
		lhs = *argv;
		rhs = strchr(*argv, '=');
		if (rhs) {
			*rhs++ = '\0';
			if (sscanf(rhs, "%u", &vol) != 1) {
				fprintf(stderr, "%s: not a number\n", lhs);
				return 1;
			}
			if (vol > 127) {
				fprintf(stderr, "%u: not in 0..127\n", vol);
				return 1;
			}
		}
		if (strlen(lhs) == 0) {
			fprintf(stderr, "stream name expected\n");
			return 1;
		}
		if (master >= 0 && strcmp(lhs, "master") == 0) {
			if (rhs)
				setmaster(vol);
			else
				printf("master=%u\n", master);
			continue;
		}
		for (cn = 0; ; cn++) {
			if (cn == MIDI_NCHAN) {
				fprintf(stderr, "%s: no such stream\n", lhs);
				return 1;
			}
			if (strcmp(lhs, ctls[cn].name) == 0)
				break;
		}
		if (rhs)
			setvol(cn, vol);
		else
			printf("%s=%u\n", ctls[cn].name, ctls[cn].vol);
	}
	mio_close(hdl);
	return 0;
}
