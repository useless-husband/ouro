// Static initialisation: scalars, arrays, structs, designators,
// strings and address constants.
#include <stdio.h>

struct Point { int x, y; };
struct Shape { const char *name; struct Point pts[3]; int npts; };

int counter = 5;
static long table[8] = {1, 2, 3, [6] = 60, 70};
char greeting[] = "hello";
char sized[8] = "abc";
const char *msg = "pointer to literal";
int *pcounter = &counter;
long *ptable_end = &table[7];
struct Point origin = {.y = 2, .x = 1};
struct Shape tri = {"triangle", {{0, 0}, {4, 0}, {0, 3}}, 3};
struct Shape shapes[] = {{"a", {{1, 1}}, 1}, [2] = {.name = "c", .npts = 2}};
int matrix[2][3] = {1, 2, 3, 4, 5, 6};
int zeros[1000];
double ratios[] = {0.5, 1.0 / 3, 2e-3};
unsigned char bytes[] = {255, 0x7f, 'a'};
static int (*fp)(const char *, ...) = printf;
int tentative;
int tentative;

static int next_id(void) {
  static int id = 100;
  return id++;
}

int main(void) {
  printf("%d %ld %ld %ld %ld\n", counter, table[0], table[3], table[6], table[7]);
  printf("%s %zu %s %zu\n", greeting, sizeof greeting, sized, sizeof sized);
  printf("%s %d %ld\n", msg, *pcounter, *ptable_end);
  printf("%d %d\n", origin.x, origin.y);
  printf("%s %d %d %d\n", tri.name, tri.pts[1].x, tri.pts[2].y, tri.npts);
  printf("%zu %s %s %d\n", sizeof shapes / sizeof shapes[0], shapes[0].name, shapes[2].name,
         shapes[2].npts);
  printf("%d %d\n", matrix[1][0], matrix[0][2]);
  printf("%d %g %g %d\n", zeros[999], ratios[1], ratios[2], bytes[0] + bytes[1] + bytes[2]);
  fp("via pointer %d\n", 7);
  printf("%d %d %d\n", next_id(), next_id(), tentative);
  struct Point local = {3};
  int arr[5] = {[3] = 9, 1};
  printf("%d %d %d %d %d\n", local.x, local.y, arr[0], arr[3], arr[4]);
  return 0;
}
