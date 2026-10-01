/* ouro: <stddef.h> */
#ifndef _OURO_STDDEF_H
#define _OURO_STDDEF_H
#ifndef _OURO_SIZE_T
#define _OURO_SIZE_T
typedef unsigned long size_t;
#endif
#ifndef _OURO_WCHAR_T
#define _OURO_WCHAR_T
typedef int wchar_t;
#endif
typedef long ptrdiff_t;
typedef long double max_align_t;
#ifndef NULL
#define NULL ((void *)0)
#endif
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
