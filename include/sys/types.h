/* ouro: <sys/types.h> */
#ifndef _OURO_SYS_TYPES_H
#define _OURO_SYS_TYPES_H
#ifndef _OURO_SIZE_T
#define _OURO_SIZE_T
typedef unsigned long size_t;
#endif
#ifndef _OURO_SSIZE_T
#define _OURO_SSIZE_T
typedef long ssize_t;
#endif
#ifndef _OURO_OFF_T
#define _OURO_OFF_T
typedef long long off_t;
#endif
#ifndef _OURO_TIME_T
#define _OURO_TIME_T
typedef long time_t;
#endif
typedef int pid_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned short mode_t;
typedef int dev_t;
typedef unsigned long long ino_t;
typedef unsigned short nlink_t;
typedef long long blkcnt_t;
typedef int blksize_t;
typedef unsigned int useconds_t;
typedef long suseconds_t;
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
#endif
