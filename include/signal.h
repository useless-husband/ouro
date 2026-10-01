/* ouro: <signal.h> */
#ifndef _OURO_SIGNAL_H
#define _OURO_SIGNAL_H
typedef int sig_atomic_t;
typedef void (*__sighandler_t)(int);
#define SIG_DFL ((__sighandler_t)0)
#define SIG_IGN ((__sighandler_t)1)
#define SIG_ERR ((__sighandler_t)-1)
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGFPE 8
#define SIGKILL 9
#define SIGBUS 10
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 20
#define SIGUSR1 30
#define SIGUSR2 31
typedef unsigned int sigset_t;
struct __siginfo;
struct sigaction {
  union {
    void (*__sa_handler)(int);
    void (*__sa_sigaction)(int, struct __siginfo *, void *);
  } __sigaction_u;
  sigset_t sa_mask;
  int sa_flags;
};
#define sa_handler __sigaction_u.__sa_handler
#define sa_sigaction __sigaction_u.__sa_sigaction
#define SA_ONSTACK 0x0001
#define SA_RESTART 0x0002
#define SA_RESETHAND 0x0004
#define SA_NODEFER 0x0010
#define SA_SIGINFO 0x0040
#define SIG_BLOCK 1
#define SIG_UNBLOCK 2
#define SIG_SETMASK 3
__sighandler_t signal(int, __sighandler_t);
int sigaction(int, const struct sigaction *restrict, struct sigaction *restrict);
int sigemptyset(sigset_t *);
int sigfillset(sigset_t *);
int sigaddset(sigset_t *, int);
int sigdelset(sigset_t *, int);
int sigismember(const sigset_t *, int);
int sigprocmask(int, const sigset_t *restrict, sigset_t *restrict);
int raise(int);
int kill(int, int);
#endif
