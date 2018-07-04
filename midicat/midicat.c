/*
 * Copyright (c) 2015 Alexandre Ratchov <alex@caoua.org>
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
#include <signal.h>
#include <sndio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "smf.h"
#include "bsd-compat.h"

#define MIDI_BUFSZ	1024

char usagestr[] = "usage: midicat [-d] [-i in-file] [-o out-file] "
	"[-q in-port] [-q out-port]\n";

char *port0, *port1, *ifile, *ofile;
struct mio_hdl *ih, *oh;
unsigned char buf[MIDI_BUFSZ];
int buf_used = 0;
int ifd = -1, ofd = -1;
int dump;

static int
midi_flush(void)
{
	int i, n, sep;

	if (buf_used == 0)
		return 1;

	if (ofile != NULL) {
		n = write(ofd, buf, buf_used);
		if (n != buf_used) {
			fprintf(stderr, "%s: short write\n", ofile);
			buf_used = 0;
			return 0;
		}
	} else {
		n = mio_write(oh, buf, buf_used);
		if (n != buf_used) {
			fprintf(stderr, "%s: port disconnected\n",
			   ih == oh ? port0 : port1);
			buf_used = 0;
			return 0;
		}
	}

	if (dump) {
		for (i = 0; i < buf_used; i++) {
			sep = (i % 16 == 15 || i == buf_used - 1) ?
			    '\n' : ' ';
			fprintf(stderr, "%02x%c", buf[i], sep);
		}
	}

	buf_used = 0;
	return 1;
}

static int
midi_send(void *arg, unsigned int val)
{
	buf[buf_used++] = val;
	if (buf_used == MIDI_BUFSZ)
		return midi_flush();
	return 1;
}

static void
sigalrm(int s)
{
}

int
main(int argc, char **argv)
{
	long long clock_nsec, delta_nsec;
	struct timespec ts, ts_last;
	struct sigaction sa;
	struct itimerval it;
	struct smf *smf;
	char *ext;
	int c, mode;
	sigset_t sigset;

	smf = NULL;
	while ((c = getopt(argc, argv, "di:o:q:")) != -1) {
		switch (c) {
		case 'd':
			dump = 1;
			break;
		case 'q':
			if (port0 == NULL)
				port0 = optarg;
			else if (port1 == NULL)
				port1 = optarg;
			else {
				fputs("too many -q options\n", stderr);
				return 1;
			}
			break;
		case 'i':
			ifile = optarg;
			break;
		case 'o':
			ofile = optarg;
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

	/* we don't support more than one data flow */
	if (ifile != NULL && ofile != NULL) {
		fputs("-i and -o are exclusive\n", stderr);
		return 1;
	}

	/* second port makes sense only for port-to-port transfers */
	if (port1 != NULL && !(ifile == NULL && ofile == NULL)) {
		fputs("too many -q options\n", stderr);
		return 1;
	}

	/* if there're neither files nor ports, then we've nothing to do */
	if (port0 == NULL && ifile == NULL && ofile == NULL)
		goto bad_usage;

	/* if no port specified, use default one */
	if (port0 == NULL)
		port0 = MIO_PORTANY;

	/* open input or output file (if any) */
	if (ifile) {
		if (strcmp(ifile, "-") == 0)
			ifd = STDIN_FILENO;
		else {
			ext = strrchr(ifile, '.');
			if (ext != NULL && strcasecmp(ext + 1, "mid") == 0) {
				smf = smf_open(ifile, midi_send, NULL);
				if (smf == NULL) {
					fprintf(stderr,
					    "%s: couldn't open file\n", ifile);
					return 1;
				}
			} else {
				ifd = open(ifile, O_RDONLY, 0);
				if (ifd < 0) {
					perror(ifile);
					return 1;
				}
			}
		}
	} else if (ofile) {
		if (strcmp(ofile, "-") == 0)
			ofd = STDOUT_FILENO;
		else {
			ofd = open(ofile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
			if (ofd < 0) {
				perror(ofile);
				return 1;
			}
		}
	}

	/* open first port for input and output (if output needed) */
	if (ofile)
		mode = MIO_IN;
	else if (ifile)
		mode = MIO_OUT;
	else if (port1 == NULL)
		mode = MIO_IN | MIO_OUT;
	else
		mode = MIO_IN;
	ih = mio_open(port0, mode, 0);
	if (ih == NULL) {
		fprintf(stderr, "%s: couldn't open port\n", port0);
		return 1;
	}

	/* open second port, output only */
	if (port1 == NULL)
		oh = ih;
	else {
		oh = mio_open(port1, MIO_OUT, 0);
		if (oh == NULL) {
			fprintf(stderr, "%s: couldn't open port\n", port1);
			exit(1);
		}
	}

	sa.sa_flags = SA_RESTART;
	sa.sa_handler = sigalrm;
	sigfillset(&sa.sa_mask);
	sigaction(SIGALRM, &sa, NULL);

	it.it_interval.tv_sec = it.it_value.tv_sec = 0;
	it.it_interval.tv_usec = it.it_value.tv_usec = 1000;
	setitimer(ITIMER_REAL, &it, NULL);

	if (clock_gettime(CLOCK_MONOTONIC, &ts_last) < 0) {
		fprintf(stderr, "CLOCK_MONOTONIC not supported\n");
		return 1;
	}

	/* make write() and mio_write() return error */
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGPIPE);
	sigprocmask(SIG_BLOCK, &sigset, NULL);

	/* transfer until end-of-file or error */
	clock_nsec = delta_nsec = 0;
	for (;;) {
		if (smf != NULL) {
			if (!smf_play(smf, &delta_nsec))
				break;
			if (!midi_flush())
				break;
			if (delta_nsec == 0)
				break;
		} else if (ifile != NULL) {
			buf_used = read(ifd, buf, sizeof(buf));
			if (buf_used < 0) {
				perror("stdin");
				break;
			}
			if (buf_used == 0)
				break;
			if (!midi_flush())
				break;
		} else {
			buf_used = mio_read(ih, buf, sizeof(buf));
			if (buf_used == 0) {
				fprintf(stderr, "%s: disconnected\n", port0);
				break;
			}
			if (!midi_flush())
				break;
		}

		/*
		 * wait delta ticks (for .mid files only)
		 */
		while (clock_nsec < delta_nsec) {
			pause();
			clock_gettime(CLOCK_MONOTONIC, &ts);
			clock_nsec += ts.tv_nsec - ts_last.tv_nsec +
			    1000000000L * (ts.tv_sec - ts_last.tv_sec);
			ts_last = ts;
		}
		clock_nsec -= delta_nsec;
	}

	/* clean-up */
	if (port0)
		mio_close(ih);
	if (port1)
		mio_close(oh);
	if (ifile) {
		if (smf)
			smf_close(smf);
		else
			close(ifd);
	}
	if (ofile)
		close(ofd);
	return 0;
}
