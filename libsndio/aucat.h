#ifndef AUCAT_H
#define AUCAT_H

#include "amsg.h"

struct sio_hdl;
struct mio_hdl;

struct aucat {
	int fd;				/* socket */
	struct amsg rmsg, wmsg;		/* temporary messages */
	size_t wtodo, rtodo;		/* bytes to complete the packet */
#define STATE_RMSG	0		/* message being received */
#define STATE_RDATA	1		/* data being received */
	unsigned rstate;		/* one of above */
#define STATE_WIDLE	2		/* nothing to do */
#define STATE_WMSG	3		/* message being transferred */
#define STATE_WDATA	4		/* data being transferred */
	unsigned wstate;		/* one of above */
};

int aucat_rmsg(struct aucat *, int *);
int aucat_wmsg(struct aucat *, int *);
size_t aucat_rdata(struct aucat *, void *, size_t, int *);
size_t aucat_wdata(struct aucat *, const void *, size_t, unsigned, int *);
int aucat_open(struct aucat *, const char *, char *, unsigned, int);
void aucat_close(struct aucat *, int);
int aucat_pollfd(struct aucat *, struct pollfd *, int);
int aucat_revents(struct aucat *, struct pollfd *);

#endif /* !defined(AUCAT_H) */
