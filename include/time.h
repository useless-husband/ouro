/* ouro: <time.h> */
#ifndef _OURO_TIME_H
#define _OURO_TIME_H
#ifndef _OURO_SIZE_T
#define _OURO_SIZE_T
typedef unsigned long size_t;
#endif
#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef _OURO_TIME_T
#define _OURO_TIME_T
typedef long time_t;
#endif
typedef unsigned long clock_t;
#define CLOCKS_PER_SEC ((clock_t)1000000)
#ifndef _OURO_TIMESPEC
#define _OURO_TIMESPEC
struct timespec {
  time_t tv_sec;
  long tv_nsec;
};
#endif
struct tm {
  int tm_sec;
  int tm_min;
  int tm_hour;
  int tm_mday;
  int tm_mon;
  int tm_year;
  int tm_wday;
  int tm_yday;
  int tm_isdst;
  long tm_gmtoff;
  char *tm_zone;
};
typedef enum {
  _CLOCK_REALTIME = 0,
  _CLOCK_MONOTONIC = 6,
  _CLOCK_MONOTONIC_RAW = 4,
  _CLOCK_PROCESS_CPUTIME_ID = 12,
  _CLOCK_THREAD_CPUTIME_ID = 16
} clockid_t;
#define CLOCK_REALTIME _CLOCK_REALTIME
#define CLOCK_MONOTONIC _CLOCK_MONOTONIC
#define CLOCK_MONOTONIC_RAW _CLOCK_MONOTONIC_RAW
#define CLOCK_PROCESS_CPUTIME_ID _CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID _CLOCK_THREAD_CPUTIME_ID

clock_t clock(void);
time_t time(time_t *);
double difftime(time_t, time_t);
time_t mktime(struct tm *);
struct tm *gmtime(const time_t *);
struct tm *localtime(const time_t *);
struct tm *gmtime_r(const time_t *restrict, struct tm *restrict);
struct tm *localtime_r(const time_t *restrict, struct tm *restrict);
char *asctime(const struct tm *);
char *ctime(const time_t *);
size_t strftime(char *restrict, size_t, const char *restrict, const struct tm *restrict);
int nanosleep(const struct timespec *, struct timespec *);
int clock_gettime(clockid_t, struct timespec *);
void tzset(void);
#endif
