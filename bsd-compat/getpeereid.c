/*
 * Copyright (c) 2010 Jacob Meuser <jakemsr@sdf.lonestar.org>
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
#include <sys/types.h>
#include <sys/socket.h>
#include "bsd-compat.h"

#ifndef HAVE_GETPEEREID
int
getpeereid(int s, uid_t *ruid, gid_t *rgid)
{
	struct ucred cr;
	socklen_t len = sizeof(cr);

	if (getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &len) < 0)
		return -1;
	*ruid = cr.uid;
	*rgid = cr.gid;
	return 0;
}
#endif
