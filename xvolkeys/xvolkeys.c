/*	$OpenBSD$	*/
/*
 * Copyright (c) 2014 Alexandre Ratchov <alex@caoua.org>
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
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sndio.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "sysex.h"

/*
 * X keysyms to increment / decrement volume: + and -
 */
#define KEY_INC	XK_plus
#define KEY_DEC	XK_minus

/*
 * modifiers: Alt + Ctrl
 */
#define MODMASK		(Mod1Mask | ControlMask)

/*
 * max MIDI message length we accept
 */
#define MIDI_MSGMAX	(sizeof(struct sysex))

/*
 * max MIDI volume
 */
#define MIDI_MAXVOL	127

/*
 * midi parser state
 */
char *port;
struct pollfd pfds[16];
struct mio_hdl *hdl;
unsigned int midx;
unsigned char mev[MIDI_MSGMAX];

int master = MIDI_MAXVOL;
int verbose;

unsigned char dumpreq[] = {
	SYSEX_START,
	SYSEX_TYPE_EDU,
	0,
	SYSEX_AUCAT,
	SYSEX_AUCAT_DUMPREQ,
	SYSEX_END
};

/*
 * X bits
 */
Display	*dpy;
KeyCode inc_code, dec_code;
KeySym *inc_map, *dec_map;

/*
 * connect to sndiod
 */
int
midi_connect(void)
{
	if (hdl != NULL)
		return;
	hdl = mio_open(port, MIO_IN | MIO_OUT, 0);
	if (hdl == NULL) {
		if (verbose)
			fprintf(stderr, "%s: couldn't open MIDI port\n", port);
		return 0;
	}
	return 1;
}

/*
 * if there's an error, close connection to sndiod
 */
void
midi_disconnect(void)
{
	if (!mio_eof(hdl))
		return;
	if (verbose)
		fprintf(stderr, "%s: MIDI port disconnected\n", port);
	mio_close(hdl);
	hdl = NULL;
}

/*
 * send master volume message and to the server
 */
void
midi_setvol(int vol)
{
	struct sysex msg;

	if (vol > MIDI_MAXVOL)
		vol = MIDI_MAXVOL;
	if (vol < 0)
		vol = 0;
	if (verbose)
		fprintf(stderr, "%s: setting volume to %d\n", port, vol);
	if (!midi_connect())
		return;
	if (master != vol) {
		master = vol;
		msg.start = SYSEX_START;
		msg.type = SYSEX_TYPE_RT;
		msg.dev = SYSEX_DEV_ANY;
		msg.id0 = SYSEX_CONTROL;
		msg.id1 = SYSEX_MASTER;
		msg.u.master.fine = 0;
		msg.u.master.coarse = vol;
		msg.u.master.end = SYSEX_END;
		if (hdl) {
			mio_write(hdl, &msg, SYSEX_SIZE(master));
			midi_disconnect();
		}
	}
}

/*
 * decode midi sysex message
 */
void
midi_sysex(unsigned char *msg, unsigned len)
{
	if (len == 8 &&
	    msg[1] == SYSEX_TYPE_RT &&
	    msg[3] == SYSEX_CONTROL &&
	    msg[4] == SYSEX_MASTER) {
		if (master != msg[6]) {
			master = msg[6];
			if (verbose) {
				fprintf(stderr, "%s: volume is %d\n",
				    port, master);
			}
		}
	}
}

/*
 * decode midi bytes
 */
void
midi_parse(unsigned char *mbuf, unsigned len)
{
	unsigned c;

	for (; len > 0; len--) {
		c = *mbuf++;
		if (c >= 0xf8) {
			/* ignore */
		} else if (c == SYSEX_END) {
			if (mev[0] == SYSEX_START) {
				mev[midx++] = c;
				midi_sysex(mev, midx);
			}
			mev[0] = 0;
		} else if (c == SYSEX_START) {
			mev[0] = c;
			midx = 1;
		} else if (c >= 0x80) {
			mev[0] = 0;
		} else if (mev[0]) {
			if (midx < MIDI_MSGMAX - 1) {
				mev[midx++] = c;
			} else
				mev[0] = 0;
		}
	}
}

/*
 * register hot-keys in X
 */
void
grab_keys(void)
{
	unsigned int i, scr, nscr;
	int nret;

	if (verbose)
		fprintf(stderr, "grabbing keys\n");
	inc_code = XKeysymToKeycode(dpy, KEY_INC);
	inc_map = XGetKeyboardMapping(dpy, inc_code, 1, &nret);
	if (nret <= ShiftMask) {
		fprintf(stderr, "couldn't get keymap for '+' key\n");
		exit(1);
	}

	dec_code = XKeysymToKeycode(dpy, KEY_DEC);
	dec_map = XGetKeyboardMapping(dpy, dec_code, 1, &nret);
	if (nret <= ShiftMask) {
		fprintf(stderr, "couldn't get keymap for '-' key\n");
		exit(1);
	}

	nscr = ScreenCount(dpy);
	for (i = 0; i <= 0xff; i++) {
		if ((i & MODMASK) != 0)
			continue;
		for (scr = 0; scr != nscr; scr++) {

			XGrabKey(dpy, inc_code, i | MODMASK,
			    RootWindow(dpy, scr), 1,
			    GrabModeAsync, GrabModeAsync);
			XGrabKey(dpy, dec_code, i | MODMASK,
			    RootWindow(dpy, scr), 1,
			    GrabModeAsync, GrabModeAsync);
		}
	}
}

/*
 * unregister hot-keys
 */
void
ungrab_keys(void)
{
	unsigned int scr, nscr;

	if (verbose)
		fprintf(stderr, "ungrabbing keys\n");

	XFree(inc_map);
	XFree(dec_map);

	nscr = ScreenCount(dpy);
	for (scr = 0; scr != nscr; scr++)
		XUngrabKey(dpy, AnyKey, AnyModifier, RootWindow(dpy, scr));
}

void
usage(void)
{
	fprintf(stderr, "usage: xvolkeys [-Dv] [-q port]\n");
	exit(1);
}

int
main(int argc, char **argv)
{
#define MIDIBUFSZ 0x100
	unsigned char msg[MIDIBUFSZ];
	unsigned int scr;
	XEvent xev;
	int c, nfds, n;
	int background, revents;

	/*
	 * parse command line options
	 */
	port = "snd/0";
	verbose = 0;
	background = 0;
	while ((c = getopt(argc, argv, "Dq:v")) != -1) {
		switch (c) {
		case 'D':
			background = 1;
			break;
		case 'v':
			verbose++;
			break;
		case 'q':
			port = optarg;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc > 0)
		usage();

	dpy = XOpenDisplay(NULL);
	if (dpy == 0) {
		fprintf(stderr, "Couldn't open display\n");
		exit(1);
	}

	/* mask non-key events for each screan */
	for (scr = 0; scr != ScreenCount(dpy); scr++)
		XSelectInput(dpy, RootWindow(dpy, scr), KeyPress);

	(void)midi_connect();

	/*
	 * request initial volume
	 */
	if (hdl) {
		mio_write(hdl, dumpreq, sizeof(dumpreq));
		midi_disconnect();
	}

	grab_keys();

	if (background) {
		verbose = 0;
		if (daemon(0, 0) < 0) {
			perror("daemon");
			exit(1);
		}
	}

	while (1) {
		while (XPending(dpy)) {
			XNextEvent(dpy, &xev);
			if (xev.type == MappingNotify) {
				if (xev.xmapping.request != MappingKeyboard)
					continue;
				if (verbose)
					fprintf(stderr, "keyboard remapped\n");
				ungrab_keys();
				grab_keys();
			}
			if (xev.type != KeyPress)
				continue;
			if (xev.xkey.keycode == inc_code &&
			    inc_map[xev.xkey.state & ShiftMask] == KEY_INC) {
				midi_setvol(master + 5);
			} else if (xev.xkey.keycode == dec_code &&
			    dec_map[xev.xkey.state & ShiftMask] == KEY_DEC) {
				midi_setvol(master - 5);
			}
		}
		nfds = (hdl != NULL) ? mio_pollfd(hdl, pfds, POLLIN) : 0;
		pfds[nfds].fd = ConnectionNumber(dpy);
		pfds[nfds].events = POLLIN;
		while (poll(pfds, nfds + 1, -1) < 0 && errno == EINTR)
			; /* nothing */
		if (hdl) {
			revents = mio_revents(hdl, pfds);
			if (revents & POLLHUP)
				midi_disconnect();
			else if (revents & POLLIN) {
				n = mio_read(hdl, msg, MIDIBUFSZ);
				midi_parse(msg, n);
			}
		}
	}
	XFree(inc_map);
	XFree(dec_map);
	XCloseDisplay(dpy);
	if (hdl)
		mio_close(hdl);
	return 0;
}
