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
#include <string.h>
#include <sndio.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

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
 * volume increment
 */
#define VOL_INC	9

char *dev_name;
struct pollfd pfds[16];
struct siomix_hdl *hdl;
unsigned int master_addr, master_val = SIOMIX_INTMAX;
int master_found = 0;
int verbose;

/*
 * X bits
 */
Display	*dpy;
KeyCode inc_code, dec_code;
KeySym *inc_map, *dec_map;

/*
 * new control
 */
static void
mixer_ondesc(void *unused, struct siomix_desc *desc, int val)
{
	if (desc == NULL)
		return;
	if (master_found)
		return;
	if (strcmp(desc->chan0.str, "master") == 0 &&
	    strcmp(desc->func, "level") == 0) {
		master_found = 1;
		master_addr = desc->addr;
		master_val = val;
		if (verbose)
			fprintf(stderr, "%s: master at addr %u, value = %u\n",
			    dev_name, master_addr, master_val);
	}
}

/*
 * control value changed
 */
static void
mixer_onctl(void *unused, unsigned int addr, unsigned int val)
{
	if (addr == master_addr) {
		if (verbose)
			fprintf(stderr, "master changed %u -> %u\n", master_val, val);
		master_val = val;
	}
}

/*
 * if there's an error, close connection to sndiod
 */
static void
mixer_disconnect(void)
{
	if (!siomix_eof(hdl))
		return;
	if (verbose)
		fprintf(stderr, "%s: mixer device disconnected\n", dev_name);
	siomix_close(hdl);
	hdl = NULL;
}

/*
 * connect to sndiod
 */
static int
mixer_connect(void)
{
	if (hdl != NULL)
		return 1;
	hdl = siomix_open(dev_name, SIOMIX_READ | SIOMIX_WRITE, 0);
	if (hdl == NULL) {
		if (verbose)
			fprintf(stderr, "%s: couldn't open mixer device\n",
			    dev_name);
		return 0;
	}
	master_found = 0;
	siomix_ondesc(hdl, mixer_ondesc, NULL);
	siomix_onctl(hdl, mixer_onctl, NULL);
	if (!master_found)
		fprintf(stderr, "%s: warning, couldn't find master control\n",
		    dev_name);
	return 1;
}

/*
 * send master volume message and to the server
 */
static void
mixer_incrvol(int incr)
{
	int vol;

	if (!mixer_connect())
		return;
	vol = master_val + incr;
	if (vol > SIOMIX_INTMAX)
		vol = SIOMIX_INTMAX;
	if (vol < 0)
		vol = 0;
	if (master_val != (unsigned int)vol) {
		master_val = vol;
		if (hdl && master_found) {
			if (verbose) {
				fprintf(stderr, "%s: setting volume to %d\n",
				    dev_name, vol);
			}
			siomix_setctl(hdl, master_addr, master_val);
			mixer_disconnect();
		}
	}
}

/*
 * register hot-keys in X
 */
static void
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
static void
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

int
main(int argc, char **argv)
{
	int scr;
	XEvent xev;
	int c, nfds;
	int background, revents;

	/*
	 * parse command line options
	 */
	dev_name = SIOMIX_DEVANY;
	verbose = 0;
	background = 0;
	while ((c = getopt(argc, argv, "Df:q:v")) != -1) {
		switch (c) {
		case 'D':
			background = 1;
			break;
		case 'v':
			verbose++;
			break;
			
		case 'q': /* compat */
		case 'f':
			dev_name = optarg;
			break;
		default:
			goto bad_usage;
		}
	}
	argc -= optind;
	argv += optind;
	if (argc > 0) {
	bad_usage:
		fprintf(stderr, "usage: xvolkeys [-Dv] [-f device]\n");
	}

	dpy = XOpenDisplay(NULL);
	if (dpy == 0) {
		fprintf(stderr, "Couldn't open display\n");
		exit(1);
	}

	/* mask non-key events for each screan */
	for (scr = 0; scr != ScreenCount(dpy); scr++)
		XSelectInput(dpy, RootWindow(dpy, scr), KeyPress);

	(void)mixer_connect();

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
				mixer_incrvol(VOL_INC);
			} else if (xev.xkey.keycode == dec_code &&
			    dec_map[xev.xkey.state & ShiftMask] == KEY_DEC) {
				mixer_incrvol(-VOL_INC);
			}
		}
		nfds = (hdl != NULL) ? siomix_pollfd(hdl, pfds, 0) : 0;
		pfds[nfds].fd = ConnectionNumber(dpy);
		pfds[nfds].events = POLLIN;
		while (poll(pfds, nfds + 1, -1) < 0 && errno == EINTR)
			; /* nothing */
		if (hdl) {
			revents = siomix_revents(hdl, pfds);
			if (revents & POLLHUP)
				mixer_disconnect();
			else if (revents & POLLIN) {
				/* what */
			}
		}
	}
	XFree(inc_map);
	XFree(dec_map);
	XCloseDisplay(dpy);
	if (hdl)
		siomix_close(hdl);
	return 0;
}
