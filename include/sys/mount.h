/* ouro: <sys/mount.h> (Darwin's 64-bit-inode struct statfs, 2168 bytes) */
#ifndef _OURO_SYS_MOUNT_H
#define _OURO_SYS_MOUNT_H
#include <sys/types.h>
#include <stdint.h>
typedef struct {
  int32_t val[2];
} fsid_t;
#define MFSTYPENAMELEN 16
#define MNAMELEN 1024
struct statfs {
  uint32_t f_bsize;
  int32_t f_iosize;
  uint64_t f_blocks;
  uint64_t f_bfree;
  uint64_t f_bavail;
  uint64_t f_files;
  uint64_t f_ffree;
  fsid_t f_fsid;
  uid_t f_owner;
  uint32_t f_type;
  uint32_t f_flags;
  uint32_t f_fssubtype;
  char f_fstypename[MFSTYPENAMELEN];
  char f_mntonname[MNAMELEN];
  char f_mntfromname[MNAMELEN];
  uint32_t f_flags_ext;
  uint32_t f_reserved[7];
};
#define MNT_RDONLY 0x00000001
#define MNT_LOCAL 0x00001000
int statfs(const char *, struct statfs *);
int fstatfs(int, struct statfs *);
#endif
