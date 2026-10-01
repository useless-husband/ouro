// Unit tests of individual compiler modules, linked against every
// source file except the driver.  Randomised tests use fixed seeds.

#include "../../src/ouro.h"

static int failures;
static int checks;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    checks++;                                                                                      \
    if (!(cond)) {                                                                                 \
      failures++;                                                                                  \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                     \
    }                                                                                              \
  } while (0)

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
static uint64_t rng(void) {
  // xorshift64*
  rng_state ^= rng_state >> 12;
  rng_state ^= rng_state << 25;
  rng_state ^= rng_state >> 27;
  return rng_state * 0x2545F4914F6CDD1DULL;
}

static void test_map_and_intern(void) {
  Map m = {0};
  for (int i = 0; i < 2000; i++)
    map_put(&m, fmt("key%d", i), (void *)(intptr_t)(i + 1));
  bool ok = true;
  for (int i = 0; i < 2000; i++)
    if (map_get(&m, fmt("key%d", i)) != (void *)(intptr_t)(i + 1))
      ok = false;
  CHECK(ok);
  CHECK(map_get(&m, "missing") == NULL);
  map_put(&m, "key7", (void *)99);
  CHECK(map_get(&m, "key7") == (void *)99);
  CHECK(m.count == 2000);

  Ident *a = intern("hello", 5);
  Ident *b = intern_cstr("hello");
  Ident *c = intern("hello world", 5);
  CHECK(a == b && a == c);
  CHECK(intern_cstr("hellp") != a);
}

static void test_arith_helpers(void) {
  CHECK(align_up(13, 8) == 16 && align_up(16, 8) == 16 && align_up(5, 1) == 5);
  CHECK(is_pow2(1) && is_pow2(1ULL << 63) && !is_pow2(0) && !is_pow2(12));
  CHECK(log2_u64(1) == 0 && log2_u64(4096) == 12 && log2_u64(1ULL << 63) == 63);
}

// DecodeBitMasks from the Arm Architecture Reference Manual: the value
// of a logical immediate with fields N, immr, imms.  Returns false for
// reserved encodings.
static bool decode_bitmask(int n, int immr, int imms, int regsize, uint64_t *out) {
  int combined = (n << 6) | (~imms & 0x3F);
  int len = -1;
  for (int i = 6; i >= 0; i--)
    if (combined & (1 << i)) {
      len = i;
      break;
    }
  if (len < 1)
    return false;
  int esize = 1 << len;
  if (esize > regsize)
    return false;
  int levels = esize - 1;
  int s = imms & levels;
  int r = immr & levels;
  if (s == levels)
    return false;
  uint64_t welem = (s + 1 == 64) ? ~0ULL : ((1ULL << (s + 1)) - 1);
  uint64_t emask = esize == 64 ? ~0ULL : ((1ULL << esize) - 1);
  uint64_t elem = r ? (((welem >> r) | (welem << (esize - r))) & emask) : welem;
  uint64_t v = 0;
  for (int i = 0; i < 64; i += esize)
    v |= elem << i;
  if (regsize == 32)
    v &= 0xFFFFFFFFULL;
  *out = v;
  return true;
}

static void test_logical_imm(void) {
  uint32_t enc;
  // Every architecturally valid immediate must be accepted.
  int valid64 = 0, valid32 = 0;
  bool all_accepted = true;
  Map seen = {0};
  for (int n = 0; n <= 1; n++)
    for (int immr = 0; immr < 64; immr++)
      for (int imms = 0; imms < 64; imms++) {
        uint64_t v;
        if (decode_bitmask(n, immr, imms, 64, &v)) {
          valid64++;
          if (!encode_logical_imm(v, 8, &enc))
            all_accepted = false;
          map_put(&seen, fmt("%llx", (unsigned long long)v), (void *)1);
        }
        if (n == 0 && decode_bitmask(n, immr, imms, 32, &v)) {
          valid32++;
          if (!encode_logical_imm(v, 4, &enc))
            all_accepted = false;
        }
      }
  CHECK(all_accepted);
  CHECK(valid64 > 5000 && valid32 > 1000);
  // Random values must be accepted exactly when they are in the set.
  bool agree = true;
  for (int i = 0; i < 200000; i++) {
    uint64_t v = rng();
    if (i % 3 == 0)
      v &= 0xFF00FF00FF00FF00ULL >> (i % 8);
    bool expect = map_get(&seen, fmt("%llx", (unsigned long long)v)) != NULL;
    if (encode_logical_imm(v, 8, &enc) != expect)
      agree = false;
  }
  CHECK(agree);
  CHECK(!encode_logical_imm(0, 8, &enc) && !encode_logical_imm(~0ULL, 8, &enc));
  CHECK(encode_logical_imm(0xFF, 4, &enc) && encode_logical_imm(0x80000001, 4, &enc));
  CHECK(!encode_logical_imm(0x1234, 8, &enc) && !encode_logical_imm(5, 8, &enc));
}

static void test_fold_int(void) {
  // Folding must agree with what C computes in the corresponding type.
  bool ok = true;
  for (int i = 0; i < 100000; i++) {
    int32_t a = (int32_t)rng(), b = (int32_t)rng();
    if (i % 4 == 0)
      b = (int32_t)(rng() % 7) - 3;
    bool okf;
    if (fold_int(IR_ADD, IR_I32, a, b, &okf) != (int32_t)((uint32_t)a + (uint32_t)b))
      ok = false;
    if (fold_int(IR_MUL, IR_I32, a, b, &okf) != (int32_t)((uint32_t)a * (uint32_t)b))
      ok = false;
    if (fold_int(IR_UDIV, IR_I32, a, b, &okf) != (b ? (int32_t)((uint32_t)a / (uint32_t)b) : 0) ||
        okf != (b != 0))
      ok = false;
    int64_t q = fold_int(IR_SDIV, IR_I32, a, b, &okf);
    if (b != 0 && !(a == INT32_MIN && b == -1) && (!okf || q != a / b))
      ok = false;
    if ((b == 0 || (a == INT32_MIN && b == -1)) && okf)
      ok = false;
    if (fold_int(IR_ASHR, IR_I32, a, b, &okf) != (a >> (b & 31)))
      ok = false;
    if (fold_int(IR_LSHR, IR_I32, a, b, &okf) != (int32_t)((uint32_t)a >> (b & 31)))
      ok = false;
    if (fold_int(IR_ULT, IR_I32, a, b, &okf) != ((uint32_t)a < (uint32_t)b))
      ok = false;
    if (fold_int(IR_SLE, IR_I32, a, b, &okf) != (a <= b))
      ok = false;
    int64_t x = (int64_t)rng(), y = (int64_t)rng();
    if (fold_int(IR_SUB, IR_I64, x, y, &okf) != (int64_t)((uint64_t)x - (uint64_t)y))
      ok = false;
    if (fold_int(IR_UREM, IR_I64, x, y, &okf) != (int64_t)((uint64_t)x % (uint64_t)y))
      ok = false;
    if (fold_int(IR_SHL, IR_I64, x, y, &okf) != (int64_t)((uint64_t)x << (y & 63)))
      ok = false;
  }
  CHECK(ok);
}

static Type *record(TypeKind kind, Type **fields, int n) {
  Type *t = new_record_type(kind, NULL, NULL);
  Member head = {0};
  Member *cur = &head;
  for (int i = 0; i < n; i++) {
    Member *m = xalloc(sizeof(Member));
    m->ty = fields[i];
    m->name = intern_cstr(fmt("m%d", i));
    cur = cur->next = m;
  }
  t->rec->members = head.next;
  layout_record(t, NULL);
  return t;
}

static void test_abi_classify(void) {
  Type *f3[] = {ty_float, ty_float, ty_float};
  ArgClass c = abi_classify(record(TY_STRUCT, f3, 3));
  CHECK(c.kind == AC_HFA && c.n == 3 && c.elem == IR_F32);
  Type *d4[] = {ty_double, ty_double, ty_double, ty_double};
  c = abi_classify(record(TY_STRUCT, d4, 4));
  CHECK(c.kind == AC_HFA && c.n == 4); // 32 bytes, still in registers
  Type *f5[] = {ty_float, ty_float, ty_float, ty_float, ty_float};
  c = abi_classify(record(TY_STRUCT, f5, 5));
  CHECK(c.kind == AC_INDIRECT); // five members: not an HFA, more than 16 bytes
  Type *fd[] = {ty_float, ty_double};
  c = abi_classify(record(TY_STRUCT, fd, 2));
  CHECK(c.kind == AC_INTS && c.n == 2); // mixed: general registers
  Type *l3[] = {ty_long, ty_long, ty_long};
  c = abi_classify(record(TY_STRUCT, l3, 3));
  CHECK(c.kind == AC_INDIRECT);
  Type *ch[] = {ty_char};
  c = abi_classify(record(TY_STRUCT, ch, 1));
  CHECK(c.kind == AC_INTS && c.n == 1);
  Type *nested_inner[] = {ty_float, ty_float};
  Type *nested[] = {record(TY_STRUCT, nested_inner, 2), ty_float};
  c = abi_classify(record(TY_STRUCT, nested, 2));
  CHECK(c.kind == AC_HFA && c.n == 3);
  CHECK(abi_classify(ty_ldouble).kind == AC_FP && abi_classify(ty_ldouble).elem == IR_F64);
  CHECK(abi_classify(pointer_to(ty_char)).kind == AC_INT);
}

static void test_layout(void) {
  Type *cdc[] = {ty_char, ty_double, ty_short};
  Type *t = record(TY_STRUCT, cdc, 3);
  CHECK(t->size == 24 && t->align == 8);
  CHECK(t->rec->members->next->offset == 8 && t->rec->members->next->next->offset == 16);
  Type *u[] = {ty_char, ty_long, ty_short};
  Type *un = record(TY_UNION, u, 3);
  CHECK(un->size == 8 && un->align == 8);
}

static void test_types(void) {
  CHECK(arith_conv(ty_int, ty_uint) == ty_uint);
  CHECK(arith_conv(ty_long, ty_uint) == ty_long);
  CHECK(arith_conv(ty_ulong, ty_llong) == ty_ullong);
  CHECK(arith_conv(ty_char, ty_ushort) == ty_int);
  CHECK(arith_conv(ty_float, ty_ulong) == ty_float);
  CHECK(promote_int(ty_bool) == ty_int && promote_int(ty_uint) == ty_uint);
  CHECK(str_eq(type_str(pointer_to(array_of(ty_int, 3))), "int (*)[3]"));
  Type *params[] = {ty_int};
  CHECK(str_eq(type_str(pointer_to(func_type(ty_void, params, 1, true, true))),
               "void (*)(int, ...)"));
}

static Token *num(const char *s) {
  Token *t = xalloc(sizeof(Token));
  t->kind = TK_PPNUM;
  t->text = (char *)s;
  t->len = strlen(s);
  convert_number(t);
  return t;
}

static void test_literals(void) {
  CHECK(num("2147483647")->ty == ty_int);
  CHECK(num("2147483648")->ty == ty_long);  // decimal: never unsigned int
  CHECK(num("0x80000000")->ty == ty_uint);  // hex: unsigned int fits first
  CHECK(num("0xFFFFFFFFFFFFFFFF")->ty == ty_ulong);
  CHECK(num("10ull")->ty == ty_ullong && num("7LU")->ty == ty_ulong);
  CHECK(num("0b101")->ival == 5 && num("017")->ival == 15);
  CHECK(num("1.5f")->ty == ty_float && num("1e3")->fval == 1000.0);
  CHECK(num("0x1p-2")->fval == 0.25);
}

int main(void) {
  type_init();
  test_map_and_intern();
  test_arith_helpers();
  test_logical_imm();
  test_fold_int();
  test_abi_classify();
  test_layout();
  test_types();
  test_literals();
  printf("unit: %d checks, %d failed\n", checks, failures);
  return failures != 0;
}
