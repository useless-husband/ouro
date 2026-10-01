/* ouro: <sys/param.h> (minimal) */
#ifndef _OURO_SYS_PARAM_H
#define _OURO_SYS_PARAM_H
#include <sys/types.h>
#include <limits.h>
#define MAXPATHLEN PATH_MAX
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
