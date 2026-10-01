/* ouro: <utime.h> */
#ifndef _OURO_UTIME_H
#define _OURO_UTIME_H
#include <sys/types.h>
struct utimbuf {
  time_t actime;
  time_t modtime;
};
int utime(const char *, const struct utimbuf *);
#endif
