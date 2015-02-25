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
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sndio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "bsd-compat.h"

char usagestr[] = "usage: midicat [-d] [-i port] [-o port]\n";

int
main(int argc, char **argv)
{
#define MIDI_BUFSZ	1024
	unsigned char buf[MIDI_BUFSZ];
	struct mio_hdl *ih, *oh;
	char *in, *out;
	int dump, conn, c, i, len, sep;

	dump = 0;
	in = NULL;
	out = NULL;
	ih = NULL;
	oh = NULL;
	
	while ((c = getopt(argc, argv, "di:o:")) != -1) {
		switch (c) {
		case 'd':
			dump = 1;
			break;
		case 'i':
			in = optarg;
			break;
		case 'o':
			out = optarg;
			break;
		default:
			goto bad_usage;
		}
	}
	argc -= optind;
	argv += optind;
	if (argc != 0) {
	bad_usage:
		fputs(usagestr, stderr);
		return 1;
	}
	if (in == NULL && out == NULL) {
		fputs("either -i or -o required\n", stderr);
		exit(1);
	}
	if (in) {
		ih = mio_open(in, MIO_IN, 0);
		if (ih == NULL) {
			fprintf(stderr, "%s: couldn't open MIDI in\n", in);
			exit(1);
		}
	}
	if (out) {
		oh = mio_open(out, MIO_OUT, 0);
		if (oh == NULL) {
			fprintf(stderr, "%s: couldn't open MIDI out\n", out);
			exit(1);
		}
	}
	for (;;) {
		if (in) {
			len = mio_read(ih, buf, sizeof(buf));
			if (len == 0) {
				fprintf(stderr, "%s: disconnected\n", in);
				break;
			}
		} else {
			len = read(STDIN_FILENO, buf, sizeof(buf));
			if (len == 0)
				break;
			if (len < 0) {
				perror("stdin");
				exit(1);
			}
		}
		if (out)
			mio_write(oh, buf, len);
		else
			write(STDOUT_FILENO, buf, len);
		if (dump) {
			for (i = 0; i < len; i++) {
				sep = (i % 16 == 15 || i == len - 1) ?
				    '\n' : ' ';
				fprintf(stderr, "%02x%c", buf[i], sep);
			}
		}
	}
	if (in)
		mio_close(ih);
	if (out)
		mio_close(oh);
	return 0;
}
