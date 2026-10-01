/* ouro: <math.h>.  Classification macros are written in plain C so
   that they need no compiler builtins. */
#ifndef _OURO_MATH_H
#define _OURO_MATH_H
#define HUGE_VAL __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define HUGE_VALL __builtin_huge_val()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#define FP_NAN 1
#define FP_INFINITE 2
#define FP_ZERO 3
#define FP_NORMAL 4
#define FP_SUBNORMAL 5
#define MATH_ERRNO 1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERREXCEPT
typedef float float_t;
typedef double double_t;
int __fpclassifyd(double);
#define fpclassify(x) __fpclassifyd((double)(x))
#define isnan(x) ((x) != (x))
#define isinf(x) (!isnan(x) && isnan((x) - (x)))
#define isfinite(x) (!isnan((x) - (x)))
#define isnormal(x) (fpclassify(x) == FP_NORMAL)
#define signbit(x) (copysign(1.0, (double)(x)) < 0)
#define M_E 2.71828182845904523536
#define M_LOG2E 1.44269504088896340736
#define M_LOG10E 0.434294481903251827651
#define M_LN2 0.693147180559945309417
#define M_LN10 2.30258509299404568402
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.785398163397448309616
#define M_1_PI 0.318309886183790671538
#define M_2_PI 0.636619772367581343076
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.707106781186547524401

double acos(double);
double asin(double);
double atan(double);
double atan2(double, double);
double cos(double);
double sin(double);
double tan(double);
double cosh(double);
double sinh(double);
double tanh(double);
double acosh(double);
double asinh(double);
double atanh(double);
double exp(double);
double exp2(double);
double expm1(double);
double frexp(double, int *);
double ldexp(double, int);
double log(double);
double log10(double);
double log2(double);
double log1p(double);
double logb(double);
double modf(double, double *);
double scalbn(double, int);
double cbrt(double);
double fabs(double);
double hypot(double, double);
double pow(double, double);
double sqrt(double);
double erf(double);
double erfc(double);
double lgamma(double);
double tgamma(double);
double ceil(double);
double floor(double);
double nearbyint(double);
double rint(double);
long lrint(double);
long long llrint(double);
double round(double);
long lround(double);
long long llround(double);
double trunc(double);
double fmod(double, double);
double remainder(double, double);
double copysign(double, double);
double nan(const char *);
double nextafter(double, double);
double fdim(double, double);
double fmax(double, double);
double fmin(double, double);
double fma(double, double, double);
int ilogb(double);

float acosf(float);
float asinf(float);
float atanf(float);
float atan2f(float, float);
float cosf(float);
float sinf(float);
float tanf(float);
float expf(float);
float logf(float);
float log10f(float);
float log2f(float);
float powf(float, float);
float sqrtf(float);
float fabsf(float);
float ceilf(float);
float floorf(float);
float roundf(float);
float truncf(float);
float fmodf(float, float);
float frexpf(float, int *);
float ldexpf(float, int);
float copysignf(float, float);
float hypotf(float, float);
float fmaxf(float, float);
float fminf(float, float);

long double fabsl(long double);
long double sqrtl(long double);
long double powl(long double, long double);
long double floorl(long double);
long double ceill(long double);
long double fmodl(long double, long double);
long double frexpl(long double, int *);
long double ldexpl(long double, int);
long double expl(long double);
long double logl(long double);
#endif
