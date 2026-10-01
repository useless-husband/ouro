/* ouro: <wchar.h> (minimal) */
#ifndef _OURO_WCHAR_H
#define _OURO_WCHAR_H
#ifndef _OURO_SIZE_T
#define _OURO_SIZE_T
typedef unsigned long size_t;
#endif
#ifndef _OURO_WCHAR_T
#define _OURO_WCHAR_T
typedef int wchar_t;
#endif
typedef int wint_t;
typedef union {
  char __mbstate8[128];
  long long _mbstateL;
} mbstate_t;
#define WEOF ((wint_t)-1)
size_t wcslen(const wchar_t *);
int wcscmp(const wchar_t *, const wchar_t *);
wchar_t *wcscpy(wchar_t *restrict, const wchar_t *restrict);
size_t mbrtowc(wchar_t *restrict, const char *restrict, size_t, mbstate_t *restrict);
size_t wcrtomb(char *restrict, wchar_t, mbstate_t *restrict);
#endif
