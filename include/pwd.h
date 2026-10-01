/* ouro: <pwd.h> (Darwin's struct passwd, 72 bytes) */
#ifndef _OURO_PWD_H
#define _OURO_PWD_H
#include <sys/types.h>
struct passwd {
  char *pw_name;
  char *pw_passwd;
  uid_t pw_uid;
  gid_t pw_gid;
  time_t pw_change;
  char *pw_class;
  char *pw_gecos;
  char *pw_dir;
  char *pw_shell;
  time_t pw_expire;
};
struct passwd *getpwuid(uid_t);
struct passwd *getpwnam(const char *);
#endif
