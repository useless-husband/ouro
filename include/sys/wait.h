/* ouro: <sys/wait.h> */
#ifndef _OURO_SYS_WAIT_H
#define _OURO_SYS_WAIT_H
#include <sys/types.h>
#define WNOHANG 1
#define WUNTRACED 2
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WTERMSIG(s) ((s) & 0177)
#define WIFEXITED(s) (((s) & 0177) == 0)
#define WIFSIGNALED(s) (((s) & 0177) != 0177 && ((s) & 0177) != 0)
#define WIFSTOPPED(s) (((s) & 0177) == 0177 && ((s) >> 8) != 0x13)
pid_t wait(int *);
pid_t waitpid(pid_t, int *, int);
#endif
