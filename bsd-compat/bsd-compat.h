#ifndef BSD_COMPAT_H
#define BSD_COMPAT_H

int issetugid(void);
size_t strlcpy(char *, const char *, size_t);
size_t strlcat(char *, const char *, size_t);
long long strtonum(const char *, long long, long long, const char **);

#endif /* !defined(BSD_COMPAT_H) */
