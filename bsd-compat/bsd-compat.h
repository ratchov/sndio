#ifndef BSD_COMPAT_H
#define BSD_COMPAT_H

/*
 * XXX: these end up exported by libsndio, hide them to avoid possible
 * collisions with program functions with the same name.
 */

int issetugid(void);
size_t strlcat(char *, const char *, size_t);
size_t strlcpy(char *, const char *, size_t);
long long strtonum(const char *, long long, long long, const char **);

#endif /* !defined(BSD_COMPAT_H) */
