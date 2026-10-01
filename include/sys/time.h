/* ouro: <sys/time.h> */
#ifndef _OURO_SYS_TIME_H
#define _OURO_SYS_TIME_H
#include <sys/types.h>
struct timeval {
  time_t tv_sec;
  int tv_usec;
};
int gettimeofday(struct timeval *, void *);
#endif
