/* ouro: <unistd.h> */
#ifndef _OURO_UNISTD_H
#define _OURO_UNISTD_H
#include <sys/types.h>
#ifndef NULL
#define NULL ((void *)0)
#endif
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#define _SC_PAGESIZE 29
#define _SC_NPROCESSORS_ONLN 58
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
int close(int);
off_t lseek(int, off_t, int);
int unlink(const char *);
int rmdir(const char *);
int access(const char *, int);
int isatty(int);
int dup(int);
int dup2(int, int);
int pipe(int *);
pid_t getpid(void);
pid_t getppid(void);
pid_t fork(void);
int execv(const char *, char *const *);
int execvp(const char *, char *const *);
int execve(const char *, char *const *, char *const *);
_Noreturn void _exit(int);
char *getcwd(char *, size_t);
int chdir(const char *);
unsigned sleep(unsigned);
int usleep(useconds_t);
long sysconf(int);
int fsync(int);
int ftruncate(int, off_t);
uid_t getuid(void);
int gethostname(char *, size_t);
extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int, char *const *, const char *);
#endif
