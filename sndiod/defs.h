/*	$OpenBSD$	*/
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
#ifndef DEFS_H
#define DEFS_H

/*
 * Log levels:
 *
 * 0 - fatal errors: bugs, asserts, internal errors.
 * 1 - warnings: bugs in clients, failed allocations, non-fatal errors.
 * 2 - misc information (hardware parameters, incoming clients)
 * 3 - structural changes (new aproc structures and files stream params changes)
 * 4 - data blocks and messages
 */
extern unsigned int log_level;

/*
 * MIDI buffer size
 */
#define MIDI_BUFSZ		3125	/* 1 second at 31.25kbit/s */

/*
 * units used for MTC clock.
 */
#define MTC_SEC			2400	/* 1 second is 2400 ticks */

/*
 * device or sub-device mode, must be a superset of corresponding SIO_XXX
 * and MIO_XXX constants
 */
#define MODE_PLAY	0x01	/* allowed to play */
#define MODE_REC	0x02	/* allowed to rec */
#define MODE_MIDIOUT	0x04	/* allowed to read midi */
#define MODE_MIDIIN	0x08	/* allowed to write midi */
#define MODE_MON	0x10	/* allowed to monitor */
#define MODE_RECMASK	(MODE_REC | MODE_MON)
#define MODE_AUDIOMASK	(MODE_PLAY | MODE_REC | MODE_MON)
#define MODE_MIDIMASK	(MODE_MIDIIN | MODE_MIDIOUT)

/*
 * underrun/overrun policies, must be the same as SIO_XXX
 */
#define XRUN_IGNORE	0	/* on xrun silently insert/discard samples */
#define XRUN_SYNC	1	/* catchup to sync to the mix/sub */
#define XRUN_ERROR	2	/* xruns are errors, eof/hup buffer */

/*
 * device types
 */
#define TYPE_AUDIO	0
#define TYPE_MIDI	1

#endif /* !defined(DEFS_H) */