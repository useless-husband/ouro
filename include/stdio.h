/* ouro: <stdio.h>.  FILE is opaque; the standard streams are Darwin's
   __stdinp / __stdoutp / __stderrp. */
#ifndef _OURO_STDIO_H
#define _OURO_STDIO_H
#include <stdarg.h>
#ifndef _OURO_SIZE_T
#define _OURO_SIZE_T
typedef unsigned long size_t;
#endif
#ifndef _OURO_SSIZE_T
#define _OURO_SSIZE_T
typedef long ssize_t;
#endif
#ifndef _OURO_OFF_T
#define _OURO_OFF_T
typedef long long off_t;
#endif
#ifndef NULL
#define NULL ((void *)0)
#endif
typedef struct __sFILE FILE;
typedef long long fpos_t;

extern FILE *__stdinp;
extern FILE *__stdoutp;
extern FILE *__stderrp;
#define stdin __stdinp
#define stdout __stdoutp
#define stderr __stderrp

#define EOF (-1)
#define BUFSIZ 1024
#define FOPEN_MAX 20
#define FILENAME_MAX 1024
#define L_tmpnam 1024
#define TMP_MAX 308915776
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

FILE *fopen(const char *restrict, const char *restrict);
FILE *freopen(const char *restrict, const char *restrict, FILE *restrict);
FILE *fdopen(int, const char *);
FILE *popen(const char *, const char *);
FILE *tmpfile(void);
int fclose(FILE *);
int pclose(FILE *);
int fflush(FILE *);
void setbuf(FILE *restrict, char *restrict);
int setvbuf(FILE *restrict, char *restrict, int, size_t);
int fileno(FILE *);

int printf(const char *restrict, ...);
int fprintf(FILE *restrict, const char *restrict, ...);
int sprintf(char *restrict, const char *restrict, ...);
int snprintf(char *restrict, size_t, const char *restrict, ...);
int dprintf(int, const char *restrict, ...);
int vprintf(const char *restrict, va_list);
int vfprintf(FILE *restrict, const char *restrict, va_list);
int vsprintf(char *restrict, const char *restrict, va_list);
int vsnprintf(char *restrict, size_t, const char *restrict, va_list);
int scanf(const char *restrict, ...);
int fscanf(FILE *restrict, const char *restrict, ...);
int sscanf(const char *restrict, const char *restrict, ...);
int vscanf(const char *restrict, va_list);
int vfscanf(FILE *restrict, const char *restrict, va_list);
int vsscanf(const char *restrict, const char *restrict, va_list);

int fgetc(FILE *);
int getc(FILE *);
int getchar(void);
int ungetc(int, FILE *);
char *fgets(char *restrict, int, FILE *restrict);
int fputc(int, FILE *);
int putc(int, FILE *);
int putchar(int);
int fputs(const char *restrict, FILE *restrict);
int puts(const char *);
ssize_t getline(char **restrict, size_t *restrict, FILE *restrict);
ssize_t getdelim(char **restrict, size_t *restrict, int, FILE *restrict);
size_t fread(void *restrict, size_t, size_t, FILE *restrict);
size_t fwrite(const void *restrict, size_t, size_t, FILE *restrict);

int fseek(FILE *, long, int);
int fseeko(FILE *, off_t, int);
long ftell(FILE *);
off_t ftello(FILE *);
void rewind(FILE *);
int fgetpos(FILE *restrict, fpos_t *restrict);
int fsetpos(FILE *, const fpos_t *);
void clearerr(FILE *);
int feof(FILE *);
int ferror(FILE *);
void perror(const char *);
int remove(const char *);
int rename(const char *, const char *);
char *tmpnam(char *);
#endif
