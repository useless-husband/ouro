/* ouro: <sys/stat.h> (Darwin's 64-bit-inode struct stat, 144 bytes) */
#ifndef _OURO_SYS_STAT_H
#define _OURO_SYS_STAT_H
#include <sys/types.h>
#ifndef _OURO_TIMESPEC
#define _OURO_TIMESPEC
struct timespec {
  time_t tv_sec;
  long tv_nsec;
};
#endif
struct stat {
  dev_t st_dev;
  mode_t st_mode;
  nlink_t st_nlink;
  ino_t st_ino;
  uid_t st_uid;
  gid_t st_gid;
  dev_t st_rdev;
  struct timespec st_atimespec;
  struct timespec st_mtimespec;
  struct timespec st_ctimespec;
  struct timespec st_birthtimespec;
  off_t st_size;
  blkcnt_t st_blocks;
  blksize_t st_blksize;
  unsigned int st_flags;
  unsigned int st_gen;
  int st_lspare;
  long long st_qspare[2];
};
#define st_atime st_atimespec.tv_sec
#define st_mtime st_mtimespec.tv_sec
#define st_ctime st_ctimespec.tv_sec
#define S_IFMT 0170000
#define S_IFIFO 0010000
#define S_IFCHR 0020000
#define S_IFDIR 0040000
#define S_IFBLK 0060000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_IRWXU 0000700
#define S_IRUSR 0000400
#define S_IWUSR 0000200
#define S_IXUSR 0000100
#define S_IRWXG 0000070
#define S_IRGRP 0000040
#define S_IWGRP 0000020
#define S_IXGRP 0000010
#define S_IRWXO 0000007
#define S_IROTH 0000004
#define S_IWOTH 0000002
#define S_IXOTH 0000001
int stat(const char *, struct stat *);
int lstat(const char *, struct stat *);
int fstat(int, struct stat *);
int chmod(const char *, mode_t);
int mkdir(const char *, mode_t);
mode_t umask(mode_t);
#endif
