// Sieve of Eratosthenes plus a CRC-32 over the result (byte arrays,
// shifts and table lookups).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned table[256];

static void crc_init(void) {
  for (unsigned i = 0; i < 256; i++) {
    unsigned c = i;
    for (int k = 0; k < 8; k++)
      c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    table[i] = c;
  }
}

static unsigned crc32(const unsigned char *p, size_t n) {
  unsigned c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++)
    c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

int main(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 50000000;
  int rounds = argc > 2 ? atoi(argv[2]) : 4;
  unsigned char *composite = malloc(n + 1);
  crc_init();
  unsigned crc = 0;
  long count = 0;
  for (int r = 0; r < rounds; r++) {
    memset(composite, 0, n + 1);
    count = 0;
    for (long i = 2; i <= n; i++) {
      if (composite[i])
        continue;
      count++;
      for (long j = i * i; j <= n; j += i)
        composite[j] = 1;
    }
    crc ^= crc32(composite, n + 1);
  }
  printf("%ld primes below %d, crc %08x\n", count, n, crc);
  free(composite);
  return 0;
}
