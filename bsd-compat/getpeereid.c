#ifdef COMPAT_GETPEEREID
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/socket.h>

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
