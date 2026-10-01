/* ouro: <setjmp.h> (arm64 Darwin: _JBLEN = (14 + 8 + 2) * 2 ints) */
#ifndef _OURO_SETJMP_H
#define _OURO_SETJMP_H
typedef int jmp_buf[48];
typedef int sigjmp_buf[49];
int setjmp(jmp_buf);
_Noreturn void longjmp(jmp_buf, int);
int _setjmp(jmp_buf);
_Noreturn void _longjmp(jmp_buf, int);
int sigsetjmp(sigjmp_buf, int);
_Noreturn void siglongjmp(sigjmp_buf, int);
#endif
