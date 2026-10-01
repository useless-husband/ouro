/* ouro: <dirent.h> (Darwin's 64-bit-inode struct dirent, 1048 bytes) */
#ifndef _OURO_DIRENT_H
#define _OURO_DIRENT_H
#include <sys/types.h>
#include <stdint.h>
typedef struct __ouro_dir DIR;
struct dirent {
  uint64_t d_ino;
  uint64_t d_seekoff;
  uint16_t d_reclen;
  uint16_t d_namlen;
  uint8_t d_type;
  char d_name[1024];
};
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
DIR *opendir(const char *);
struct dirent *readdir(DIR *);
int closedir(DIR *);
void rewinddir(DIR *);
#endif
