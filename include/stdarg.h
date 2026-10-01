/* ouro: <stdarg.h>.  On arm64 macOS va_list is a pointer into the
   caller's stack-argument area: every variadic argument is passed on
   the stack. */
#ifndef _OURO_STDARG_H
#define _OURO_STDARG_H
typedef __builtin_va_list va_list;
typedef __builtin_va_list __gnuc_va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) __builtin_va_end(ap)
#define va_copy(dst, src) __builtin_va_copy(dst, src)
#define __va_copy(dst, src) __builtin_va_copy(dst, src)
#endif
