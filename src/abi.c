// Apple arm64 procedure call standard: how C types travel.
//
// The rules are AAPCS64 with Apple's documented differences ("Writing
// ARM64 code for Apple platforms"), all checked against clang:
//
//   * arguments on the stack take their natural size and alignment
//     (a char takes one byte) instead of 8-byte slots;
//   * every variadic argument goes on the stack in 8-byte slots, even
//     when argument registers are free;
//   * the caller extends integer arguments narrower than 32 bits, and
//     the callee extends narrow return values;
//   * plain char is signed, long double is double, x18 is reserved.
//
// This file only classifies types; irgen.c assigns registers and stack
// offsets on both sides of a call with the same rules.

#include "ouro.h"

// The SSA value type of an expression of C type `ty`.  Aggregates are
// handled by address, so they map to a pointer-sized integer.
int ir_type_of(Type *ty) {
  switch (ty->kind) {
  case TY_VOID:
    return IR_VOID;
  case TY_FLOAT:
    return IR_F32;
  case TY_DOUBLE:
  case TY_LDOUBLE:
    return IR_F64;
  case TY_BOOL:
  case TY_CHAR:
  case TY_SCHAR:
  case TY_UCHAR:
  case TY_SHORT:
  case TY_USHORT:
  case TY_INT:
  case TY_UINT:
    return IR_I32;
  case TY_ENUM:
    return ty->size == 8 ? IR_I64 : IR_I32;
  default:
    return IR_I64;
  }
}

// The memory type used to load or store a scalar of type `ty`.
int ir_memtype_of(Type *ty) {
  if (ty->kind == TY_FLOAT)
    return IR_F32;
  if (ty->kind == TY_DOUBLE || ty->kind == TY_LDOUBLE)
    return IR_F64;
  switch (ty->size) {
  case 1:
    return IR_I8;
  case 2:
    return IR_I16;
  case 4:
    return IR_I32;
  default:
    return IR_I64;
  }
}

// Decides whether an aggregate is a homogeneous floating-point aggregate
// (AAPCS64 5.9.5): at most four members of one floating type, counting
// through nested structs and arrays.
static bool hfa_walk(Type *ty, int *elem, int *count) {
  switch (ty->kind) {
  case TY_FLOAT:
  case TY_DOUBLE:
  case TY_LDOUBLE: {
    int e = ty->kind == TY_FLOAT ? IR_F32 : IR_F64;
    if (*elem == IR_VOID)
      *elem = e;
    else if (*elem != e)
      return false;
    (*count)++;
    return *count <= 4;
  }
  case TY_ARRAY:
    if (ty->len <= 0)
      return ty->len == 0;
    for (int64_t i = 0; i < ty->len; i++)
      if (!hfa_walk(ty->base, elem, count))
        return false;
    return true;
  case TY_STRUCT:
    for (Member *m = ty->rec->members; m; m = m->next) {
      if (m->is_bitfield)
        return false;
      if (!hfa_walk(m->ty, elem, count))
        return false;
    }
    return true;
  case TY_UNION: {
    int best = 0;
    for (Member *m = ty->rec->members; m; m = m->next) {
      int c = 0;
      if (m->is_bitfield || !hfa_walk(m->ty, elem, &c))
        return false;
      if (c > best)
        best = c;
    }
    *count += best;
    return *count <= 4;
  }
  default:
    return false;
  }
}

ArgClass abi_classify(Type *ty) {
  ArgClass c;
  c.kind = AC_NONE;
  c.n = 0;
  c.elem = IR_VOID;
  switch (ty->kind) {
  case TY_VOID:
    return c;
  case TY_FLOAT:
  case TY_DOUBLE:
  case TY_LDOUBLE:
    c.kind = AC_FP;
    c.n = 1;
    c.elem = ty->kind == TY_FLOAT ? IR_F32 : IR_F64;
    return c;
  case TY_STRUCT:
  case TY_UNION: {
    if (ty->size == 0)
      return c; // an empty struct (GNU) occupies no argument slot
    int elem = IR_VOID;
    int count = 0;
    if (hfa_walk(ty, &elem, &count) && count >= 1 && ty->size == count * ir_size(elem)) {
      c.kind = AC_HFA;
      c.n = count;
      c.elem = elem;
      return c;
    }
    if (ty->size > 16) {
      c.kind = AC_INDIRECT;
      c.n = 1;
      return c;
    }
    c.kind = AC_INTS;
    c.n = (int)((ty->size + 7) / 8);
    return c;
  }
  default:
    c.kind = AC_INT;
    c.n = 1;
    return c;
  }
}
