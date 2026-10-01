/* ouro: <assert.h> (deliberately without an include guard: the
   meaning of assert follows NDEBUG at each inclusion) */
#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
void __assert_rtn(const char *, const char *, int, const char *) __attribute__((noreturn));
#define assert(e) ((e) ? (void)0 : __assert_rtn(__func__, __FILE__, __LINE__, #e))
#endif
#ifndef static_assert
#define static_assert _Static_assert
#endif
