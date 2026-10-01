/* ouro: <stdlib.h> */
#ifndef _OURO_STDLIB_H
#define _OURO_STDLIB_H
#ifndef _OURO_SIZE_T
#define _OURO_SIZE_T
typedef unsigned long size_t;
#endif
#ifndef _OURO_WCHAR_T
#define _OURO_WCHAR_T
typedef int wchar_t;
#endif
#ifndef NULL
#define NULL ((void *)0)
#endif
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff
extern int __mb_cur_max;
#define MB_CUR_MAX ((size_t)__mb_cur_max)

typedef struct {
  int quot;
  int rem;
} div_t;
typedef struct {
  long quot;
  long rem;
} ldiv_t;
typedef struct {
  long long quot;
  long long rem;
} lldiv_t;

void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
void *aligned_alloc(size_t, size_t);
int posix_memalign(void **, size_t, size_t);
_Noreturn void abort(void);
_Noreturn void exit(int);
_Noreturn void _Exit(int);
int atexit(void (*)(void));
char *getenv(const char *);
int setenv(const char *, const char *, int);
int unsetenv(const char *);
int system(const char *);
int abs(int);
long labs(long);
long long llabs(long long);
div_t div(int, int);
ldiv_t ldiv(long, long);
lldiv_t lldiv(long long, long long);
int atoi(const char *);
long atol(const char *);
long long atoll(const char *);
double atof(const char *);
long strtol(const char *restrict, char **restrict, int);
unsigned long strtoul(const char *restrict, char **restrict, int);
long long strtoll(const char *restrict, char **restrict, int);
unsigned long long strtoull(const char *restrict, char **restrict, int);
double strtod(const char *restrict, char **restrict);
float strtof(const char *restrict, char **restrict);
long double strtold(const char *restrict, char **restrict);
int rand(void);
void srand(unsigned);
long random(void);
void srandom(unsigned);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
void *bsearch(const void *, const void *, size_t, size_t, int (*)(const void *, const void *));
int mkstemp(char *);
int mkstemps(char *, int);
char *mkdtemp(char *);
char *realpath(const char *restrict, char *restrict) __asm__("_realpath$DARWIN_EXTSN");
int mblen(const char *, size_t);
size_t mbstowcs(wchar_t *restrict, const char *restrict, size_t);
int mbtowc(wchar_t *restrict, const char *restrict, size_t);
size_t wcstombs(char *restrict, const wchar_t *restrict, size_t);
int wctomb(char *, wchar_t);
#endif
