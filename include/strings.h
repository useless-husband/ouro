/* ouro: <strings.h> */
#ifndef _OURO_STRINGS_H
#define _OURO_STRINGS_H
#ifndef _OURO_SIZE_T
#define _OURO_SIZE_T
typedef unsigned long size_t;
#endif
int strcasecmp(const char *, const char *);
int strncasecmp(const char *, const char *, size_t);
void bzero(void *, size_t);
int bcmp(const void *, const void *, size_t);
int ffs(int);
#endif
