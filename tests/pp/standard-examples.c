#define x 3
#define f(a) f(x * (a))
#undef x
#define x 2
#define g f
#define z z[0]
#define hh g(~
#define m(a) a(w)
#define w 0,1
#define t(a) a
#define p() int
#define q(x) x
#define r(x,y) x ## y
#define str(x) # x
f(y+1) + f(f(z)) % t(t(g)(0) + t)(1);
g(x+(3,4)-w) | hh 5) & m
(f)^m(m);
p() i[q()] = { q(1), r(2,3), r(4,), r(,5), r(,) };
char c[2][6] = { str(hello), str() };
#define xstr(s) str(s)
#define debug(s, t) printf("x" # s "= %d, x" # t "= %s", \
 x ## s, x ## t)
#define INCFILE(n) vers ## n
#define glue(a, b) a ## b
#define xglue(a, b) glue(a, b)
#define HIGHLOW "hello"
#define LOW LOW ", world"
debug(1, 2);
fputs(str(strncmp("abc\0d", "abc", '\4') // this goes away
 == 0) str(: @\n), s);
xstr(INCFILE(2).h)
glue(HIGH, LOW);
xglue(HIGH, LOW)
#define showlist(...) puts(#__VA_ARGS__)
#define report(test, ...) ((test)?puts(#test): printf(__VA_ARGS__))
showlist(The first, second, and third items.);
report(x>y, "x is %d but y is %d", x, y);
#define e(fmt, ...) pr(fmt, ## __VA_ARGS__)
e("a") e("b", 1, 2)
#if defined(x) && x == 2 && !defined y
yes1
#elif 1
no1
#else
no2
#endif
#if 0
#if garbage (
#endif
#else
yes2
#endif
#ifdef FROM_SYS
no3
#endif
#include "inc/guarded.h"
#ifdef FROM_SYS
yes3 __LINE__ __FILE__
#endif
#if (-1 < 0u) || (1 ? 2 : (1/0))
no4
#else
yes4
#endif
#define EMPTY
#define LPAREN (
#define RPAREN )
#define F(x, y) x + y
#define ELLIP_FUNC(...) __VA_ARGS__
ELLIP_FUNC(F, LPAREN, 'a', 'b', RPAREN);
#define OBJ (OBJ + 1)
OBJ
#define AA BB
#define BB AA
AA BB
