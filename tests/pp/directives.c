// Conditional inclusion, include guards, #pragma once, #line, __VA_OPT__,
// push/pop_macro and the operators of #if.
#include "inc/guarded.h"
#include "inc/guarded.h"
#include "inc/once.h"
#include "inc/once.h"
#define VERSION 3
#if VERSION > 2 && defined(VERSION) && !defined(UNDEFINED_THING)
version_ok
#elif 1
wrong1
#else
wrong2
#endif
#if 0
# error this is skipped
#else
else_taken
#endif
#if (-1 < 0u) || (1 ? 0 : 1/0)
unsigned_compare_and_short_circuit
#endif
#if 0x10 == 16 && 010 == 8 && 'A' == 65 && (2 * 3 + 4) % 7 == 3 && (1 << 3) == 8
arithmetic_ok
#endif
#ifdef VERSION
#undef VERSION
#endif
#ifndef VERSION
undef_ok
#endif
#define LOG(fmt, ...) printf(fmt __VA_OPT__(,) __VA_ARGS__)
LOG("a"); LOG("b %d", 1);
#define CAT(a, b) a ## b
#define XCAT(a, b) CAT(a, b)
XCAT(x, __LINE__) CAT(1, 2.5e) CAT(,) CAT(a,) CAT(,b)
#define STR(x) #x
#define XSTR(x) STR(x)
STR(  spaced   out   "q\"uote" 'c' ) XSTR(__FILE__) STR()
#line 1000 "renamed.c"
__LINE__ __FILE__
#define M 1
#pragma push_macro("M")
#undef M
#define M 2
M
#pragma pop_macro("M")
M
#define EMPTY
#define F(x) [x]
F() F(EMPTY) F((a,b)) F(F(1))
#define obj_like (1 + 2)
#define fn_like(x) (x * 2)
obj_like fn_like fn_like(3) fn_like (4)
