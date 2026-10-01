// ouro: a self-hosting C11 compiler for arm64 macOS.
//
// This header is the interface between the stages of the compiler:
//
//   lex.c    source text        -> tokens
//   pp.c     tokens             -> preprocessed tokens (macro expansion, #include, #if)
//   parse.c  tokens             -> typed AST (declarations, statements)
//   expr.c                         expressions and their type rules
//   init.c                         initializers, static data images
//   irgen.c  AST                -> SSA-form IR (locals still in memory)
//   opt*.c   IR                 -> IR (mem2reg, SCCP, combine, CSE, DCE, CFG cleanup)
//   isel.c   IR                 -> machine IR on virtual registers
//   regalloc.c                     liveness + linear-scan register allocation
//   emit.c   machine IR         -> assembly text for the system assembler
//   main.c   the driver: options, temporary files, running `as` and `ld`
//
// Memory comes from an arena and is never freed: the compiler is a
// batch process whose peak footprint is small.  Nothing iterates over a
// hash table or orders by pointer value, so the output is a pure
// function of the input; the bootstrap test depends on that.

#ifndef OURO_H
#define OURO_H

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OURO_VERSION "0.1.0"

#if defined(__GNUC__) || defined(__ouro__)
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN
#endif

#if defined(__clang__)
#define FMT(a, b) __attribute__((format(printf, a, b)))
#else
#define FMT(a, b)
#endif

typedef struct Type Type;
typedef struct Member Member;
typedef struct Record Record;
typedef struct Token Token;
typedef struct SrcFile SrcFile;
typedef struct Ident Ident;
typedef struct Macro Macro;
typedef struct Hideset Hideset;
typedef struct Sym Sym;
typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Init Init;
typedef struct Reloc Reloc;
typedef struct Binding Binding;
typedef struct Inst Inst;
typedef struct Block Block;
typedef struct Func Func;
typedef struct MInst MInst;

//
// util.c
//

void *xalloc(size_t size); // zero-filled, never freed
void *xgrow(void *old, int old_bytes, int new_bytes);
char *xstrdup(const char *s);
char *xstrndup(const char *s, int n);
char *fmt(const char *f, ...) FMT(1, 2);
bool str_eq(const char *a, const char *b);
bool str_startswith(const char *s, const char *prefix);
bool str_endswith(const char *s, const char *suffix);

typedef struct {
  char *buf;
  int len;
  int cap;
} Buf;

void buf_putc(Buf *b, int c);
void buf_put(Buf *b, const char *s, int n);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *f, ...) FMT(2, 3);
char *buf_cstr(Buf *b);

typedef struct {
  void **data;
  int len;
  int cap;
} Vec;

void vec_push(Vec *v, void *p);

// String-keyed hash table (used for file names and labels; identifiers
// are interned instead).
typedef struct {
  char **keys;
  void **vals;
  int cap;
  int count;
} Map;

void *map_get(Map *m, const char *key);
void map_put(Map *m, const char *key, void *val);

// Interned identifiers.  Every spelling maps to exactly one Ident, so
// identifiers compare by pointer.  The Ident also carries the current
// macro definition and the innermost visible declarations, which makes
// name lookup a pointer dereference instead of a hash probe per scope.
struct Ident {
  char *name;
  int len;
  int kw;           // keyword token kind, or 0
  Macro *macro;     // current #define, or NULL
  Binding *ordinary; // innermost binding in the ordinary namespace
  Binding *tag;      // innermost struct/union/enum tag binding
  Ident *hash_next;
};

Ident *intern(const char *s, int len);
Ident *intern_cstr(const char *s);

int64_t align_up(int64_t n, int64_t a);
bool is_pow2(uint64_t v);
int log2_u64(uint64_t v);

//
// diag.c
//

struct SrcFile {
  char *path;    // path used to open the file
  char *display; // name for diagnostics and __FILE__ (#line can change it)
  char *text;    // contents, NUL-terminated, line splices removed
  int id;
  int line_delta; // added to physical line numbers (#line)
  SrcFile *includer;
  int include_line;
};

extern bool opt_warnings;
extern bool opt_werror;
extern int error_count;
extern int warning_count;

NORETURN void fatal(const char *f, ...) FMT(1, 2);
NORETURN void error_tok(Token *tok, const char *f, ...) FMT(2, 3);
NORETURN void error_at(SrcFile *file, char *loc, int line, const char *f, ...) FMT(4, 5);
void warn_tok(Token *tok, const char *f, ...) FMT(2, 3);
void note_tok(Token *tok, const char *f, ...) FMT(2, 3);
NORETURN void internal_error(const char *file, int line, const char *f, ...) FMT(3, 4);
#define ICE(...) internal_error(__FILE__, __LINE__, __VA_ARGS__)
#define ASSERT(c)                                                                                  \
  do {                                                                                             \
    if (!(c))                                                                                      \
      ICE("assertion failed: %s", #c);                                                             \
  } while (0)

//
// lex.c
//

// Token kinds.  Single-character punctuators use their own character
// code, so the parser can write `tok->kind == '{'`.
enum {
  TK_EOF = 0,
  TK_IDENT = 128,
  TK_PPNUM,   // preprocessing number, converted to TK_INT / TK_FLOAT after preprocessing
  TK_CHAR,    // character constant (value in ival after conversion)
  TK_STR,     // string literal
  TK_OTHER,   // a character that is not a valid C token (allowed while preprocessing)
  TK_INT,     // integer constant
  TK_FLOAT,   // floating constant
  // multi-character punctuators
  P_ARROW,    // ->
  P_INC,      // ++
  P_DEC,      // --
  P_SHL,      // <<
  P_SHR,      // >>
  P_LE,       // <=
  P_GE,       // >=
  P_EQ,       // ==
  P_NE,       // !=
  P_ANDAND,   // &&
  P_OROR,     // ||
  P_MUL_EQ,   // *=
  P_DIV_EQ,   // /=
  P_MOD_EQ,   // %=
  P_ADD_EQ,   // +=
  P_SUB_EQ,   // -=
  P_SHL_EQ,   // <<=
  P_SHR_EQ,   // >>=
  P_AND_EQ,   // &=
  P_XOR_EQ,   // ^=
  P_OR_EQ,    // |=
  P_ELLIPSIS, // ...
  P_HASHHASH, // ##
  // keywords (assigned after preprocessing)
  KW_FIRST,
  KW_AUTO = KW_FIRST,
  KW_BREAK,
  KW_CASE,
  KW_CHAR,
  KW_CONST,
  KW_CONTINUE,
  KW_DEFAULT,
  KW_DO,
  KW_DOUBLE,
  KW_ELSE,
  KW_ENUM,
  KW_EXTERN,
  KW_FLOAT,
  KW_FOR,
  KW_GOTO,
  KW_IF,
  KW_INLINE,
  KW_INT,
  KW_LONG,
  KW_REGISTER,
  KW_RESTRICT,
  KW_RETURN,
  KW_SHORT,
  KW_SIGNED,
  KW_SIZEOF,
  KW_STATIC,
  KW_STRUCT,
  KW_SWITCH,
  KW_TYPEDEF,
  KW_UNION,
  KW_UNSIGNED,
  KW_VOID,
  KW_VOLATILE,
  KW_WHILE,
  KW_ALIGNAS,
  KW_ALIGNOF,
  KW_ATOMIC,
  KW_BOOL,
  KW_COMPLEX,
  KW_GENERIC,
  KW_IMAGINARY,
  KW_NORETURN,
  KW_STATIC_ASSERT,
  KW_THREAD_LOCAL,
  KW_ASM,
  KW_ATTRIBUTE,
  KW_TYPEOF,
  KW_EXTENSION,
  KW_BUILTIN_VA_LIST,
  KW_LAST,
};

enum {
  TF_BOL = 1,     // first token on a line
  TF_SPACE = 2,   // preceded by white space
  TF_NOEXPAND = 4 // an identifier that must never be macro-expanded again
};

struct Token {
  short kind;
  unsigned char flags;
  SrcFile *file;
  char *loc;    // where the token starts in file->text (for the caret)
  int line;
  char *text;   // spelling
  int len;
  Ident *ident; // TK_IDENT
  Hideset *hs;  // macros this token came out of (Prosser's algorithm)
  Token *origin; // outermost macro invocation that produced this token
  Token *next;

  // Literal values, filled in after preprocessing.
  int64_t ival;
  double fval;
  Type *ty;
  char *str;     // string bytes (encoded in the element width), with terminator
  int str_bytes; // size of `str` in bytes, terminator included
};

SrcFile *load_file(const char *path);
SrcFile *string_file(const char *name, char *text);
Token *lex_file(SrcFile *f);
Token *lex_string(const char *name, char *text, Token *pos_tmpl);
Token *new_token(int kind, Token *tmpl);
Token *copy_token(Token *t);
char *tok_str(Token *t);
const char *kind_name(int kind);
bool convert_number(Token *t);
void convert_char(Token *t);
void convert_string(Token *t);
void convert_keywords_and_literals(Token *t);

//
// pp.c
//

typedef struct {
  char *dir;
  bool is_system;
} IncludeDir;

extern Vec include_dirs;     // IncludeDir*
extern Vec quote_dirs;       // char*
void pp_init(void);
void pp_define(const char *def); // "NAME" or "NAME=VALUE"
void pp_undef(const char *name);
Token *preprocess_file(const char *path);
void print_preprocessed(FILE *out, Token *tok);
extern bool opt_E_linemarkers;

//
// type.c
//

typedef enum {
  TY_VOID,
  TY_BOOL,
  TY_CHAR, // plain char: signed on arm64 macOS
  TY_SCHAR,
  TY_UCHAR,
  TY_SHORT,
  TY_USHORT,
  TY_INT,
  TY_UINT,
  TY_LONG,
  TY_ULONG,
  TY_LLONG,
  TY_ULLONG,
  TY_FLOAT,
  TY_DOUBLE,
  TY_LDOUBLE, // same representation as double on this target
  TY_ENUM,
  TY_PTR,
  TY_ARRAY,
  TY_FUNC,
  TY_STRUCT,
  TY_UNION,
} TypeKind;

enum { Q_CONST = 1, Q_VOLATILE = 2, Q_RESTRICT = 4 };

struct Type {
  TypeKind kind;
  int64_t size; // -1 while incomplete
  int align;
  bool is_unsigned;
  int qual;
  Type *unqual; // the same type without qualifiers (itself if unqualified)

  Type *base;  // pointee or element type
  int64_t len; // array length, -1 if unspecified
  Type *ptr_to; // cached pointer_to(this)

  // Functions.
  Type *ret;
  Type **params;
  int nparams;
  bool variadic;
  bool has_proto;     // false for `int f()`
  Token **param_toks; // parameter names as written (NULL entries if unnamed)

  // Structs, unions and enums share the record so that qualified
  // copies refer to the same declaration.
  Record *rec;
};

struct Member {
  Member *next;
  Ident *name; // NULL for an anonymous struct/union member or unnamed bit-field
  Type *ty;
  Token *tok;
  int64_t offset;
  int idx;
  bool is_bitfield;
  int bit_offset; // within the storage unit at `offset`
  int bit_width;
};

struct Record {
  Ident *tag;
  Token *tok;
  Member *members;
  int nmembers;
  bool complete;
  bool flexible; // ends in a flexible array member
  bool packed;
  bool is_enum;
  Type *enum_base; // underlying integer type of an enum
};

extern Type *ty_void, *ty_bool, *ty_char, *ty_schar, *ty_uchar, *ty_short, *ty_ushort, *ty_int,
    *ty_uint, *ty_long, *ty_ulong, *ty_llong, *ty_ullong, *ty_float, *ty_double, *ty_ldouble;

void type_init(void);
bool is_integer(Type *t);
bool is_flonum(Type *t);
bool is_arith(Type *t);
bool is_scalar(Type *t);
bool is_ptr(Type *t);
bool is_record(Type *t);
bool is_aggregate(Type *t);
bool is_func(Type *t);
bool is_array(Type *t);
bool is_void(Type *t);
bool is_unsigned_int(Type *t);
bool is_complete(Type *t);
bool is_char_type(Type *t);
int int_rank(Type *t);
Type *pointer_to(Type *base);
Type *array_of(Type *base, int64_t len);
Type *func_type(Type *ret, Type **params, int nparams, bool variadic, bool has_proto);
Type *qualified(Type *t, int qual);
Type *unqual(Type *t);
Type *new_record_type(TypeKind kind, Ident *tag, Token *tok);
Type *enum_type(Ident *tag, Token *tok);
bool types_compatible(Type *a, Type *b);
Type *composite_type(Type *a, Type *b);
Type *promote_int(Type *t);
Type *arith_conv(Type *a, Type *b);
Type *decay(Type *t);
char *type_str(Type *t);
void layout_record(Type *t, Token *tok);
Member *find_member(Type *rec, Ident *name, Vec *path);
int64_t type_max_uint(Type *t);

//
// parse.c / expr.c / init.c
//

typedef enum {
  SC_NONE,
  SC_TYPEDEF,
  SC_EXTERN,
  SC_STATIC,
  SC_AUTO,
  SC_REGISTER,
} StorageClass;

typedef enum {
  SYM_VAR,
  SYM_FUNC,
  SYM_TYPEDEF,
  SYM_ENUMCONST,
} SymKind;

// A relocation in a static data image: the 8 bytes at `offset` hold
// the address of `sym` plus `addend`.
struct Reloc {
  Reloc *next;
  int64_t offset;
  Sym *sym;
  int64_t addend;
};

struct Sym {
  SymKind kind;
  Ident *name;
  char *asm_name; // assembler symbol
  Type *ty;
  Token *tok;
  StorageClass sc;
  bool is_local;   // automatic storage
  bool is_param;
  bool is_global;  // file scope or static local: lives in the data image
  bool is_extern_linkage;
  bool is_defined;
  bool is_tentative;
  bool is_inline;
  bool is_string;  // anonymous string literal
  bool is_used;    // referenced from emitted code or data
  bool is_addr_taken;
  int align;
  int64_t enum_val;

  // Static storage.
  char *data;     // NULL means all zero
  Reloc *relocs;
  Init *init;     // initializer of a static local (resolved at the end)

  // Functions.
  Stmt *body;
  Sym **params;
  int nparams;
  Vec locals;     // Sym* of every automatic object, parameters first
  bool calls_setjmp;
  bool has_va_start;
  bool is_knr;    // defined with an identifier list: arguments arrive promoted
  bool is_noreturn;

  // Code generation.
  Inst *slot;     // address of a local's storage

  Sym *next;      // program order
};

typedef enum {
  E_ICONST,
  E_FCONST,
  E_STR,       // string literal: an array lvalue (sym)
  E_VAR,       // sym
  E_CALL,      // l(args...)
  E_CAST,      // (ty) l, explicit or implicit
  E_DECAY,     // array or function designator l used as a pointer
  E_ADDR,      // &l
  E_DEREF,     // *l
  E_MEMBER,    // l.member (l is a struct/union lvalue or rvalue)
  E_NEG,
  E_BITNOT,
  E_NOT,
  E_ADD,
  E_SUB,
  E_MUL,
  E_DIV,
  E_MOD,
  E_SHL,
  E_SHR,
  E_BITAND,
  E_BITOR,
  E_BITXOR,
  E_EQ,
  E_NE,
  E_LT,
  E_LE,
  E_GT,
  E_GE,
  E_PTRADD,    // pointer l + integer r (r is in elements; scale = pointee size)
  E_PTRDIFF,   // (pointer l - pointer r) / scale
  E_LOGAND,
  E_LOGOR,
  E_COND,      // cond ? l : r
  E_COMMA,
  E_ASSIGN,    // l = r
  E_OPASSIGN,  // l op= r, computed in type `cty`
  E_PREINC,    // ++l / --l  (ival = +1 / -1)
  E_POSTINC,   // l++ / l--  (ival = +1 / -1)
  E_STMTEXPR,  // ({ ... })
  E_COMPLIT,   // compound literal: sym initialised with init
  E_VA_START,
  E_VA_ARG,
  E_BUILTIN_TRAP,
} ExprKind;

struct Expr {
  ExprKind kind;
  Type *ty;
  Token *tok;
  Expr *l;
  Expr *r;
  Expr *cond;
  Expr **args;
  int nargs;
  Member *member;
  Sym *sym;
  int64_t ival;
  double fval;
  int op;     // E_OPASSIGN: the binary ExprKind
  Type *cty;  // E_OPASSIGN: computation type
  Stmt *body; // E_STMTEXPR
  Init *init; // E_COMPLIT
  int64_t scale; // E_PTRADD / E_PTRDIFF
};

typedef enum {
  S_EXPR,
  S_DECL,     // local variable with initializer
  S_BLOCK,
  S_IF,
  S_WHILE,
  S_DO,
  S_FOR,
  S_SWITCH,
  S_CASE,
  S_DEFAULT,
  S_BREAK,
  S_CONTINUE,
  S_RETURN,
  S_GOTO,
  S_LABEL,
  S_NULL,
} StmtKind;

typedef struct Label Label;
struct Label {
  Ident *name;
  Token *tok;
  bool defined;
  bool used;
  Block *bb;
};

struct Stmt {
  StmtKind kind;
  Token *tok;
  Expr *expr;   // S_EXPR, S_RETURN, condition of if/while/do/for/switch
  Stmt *init;   // S_FOR
  Expr *inc;    // S_FOR
  Stmt *then;   // body of loops, if, switch, case, label
  Stmt *els;
  Stmt **stmts; // S_BLOCK
  int nstmts;
  Sym *var;     // S_DECL
  Init *vinit;  // S_DECL
  Label *label; // S_GOTO / S_LABEL
  // switch / case
  int64_t case_lo;
  int64_t case_hi;
  Stmt **cases; // S_SWITCH: S_CASE statements in source order
  int ncases;
  Stmt *default_stmt;
  Block *bb;    // irgen: block of a case label
};

// An initializer tree.  For aggregates `elems` has one slot per array
// element or struct member; NULL slots are zero.
struct Init {
  Type *ty;
  Expr *expr;
  Init **elems;
  int64_t nelems;
  Member *union_mem;
  Token *tok;
};

typedef struct {
  Sym *syms;    // every global symbol in definition order
  Vec funcs;    // Sym* of function definitions
} Program;

Program *parse_program(Token *tok);

// A declaration visible in some scope (see enter_scope / leave_scope).
struct Binding {
  Binding *shadowed;   // the binding of the same identifier in an outer scope
  Binding *scope_next; // other bindings made in the same scope
  Ident *ident;
  int depth;
  bool is_tag;
  Sym *sym;  // ordinary identifiers
  Type *tag; // struct/union/enum tags
};

// Shared by the parser modules (defined in parse.c).
typedef struct {
  Token **toks;
  int pos;
} Parser;
extern Parser P;
Token *peek(void);
Token *peek_at(int k);
Token *next_tok(void);
bool accept(int kind);
Token *expect(int kind, const char *what);
bool is_type_start(Token *t);
Type *parse_typename(void);
Expr *parse_expr(void);
Expr *parse_assign(void);
Expr *parse_cond_expr(void);
int64_t parse_const_int(void);
Init *parse_initializer(Type *ty, Type **completed);
Stmt *parse_compound_stmt_body(void);
Sym *new_local(Ident *name, Type *ty, Token *tok);
Sym *new_static_local(Ident *name, Type *ty, Token *tok);
Sym *new_string_sym(Token *str);
Sym *current_function(void);
void enter_scope(void);
void leave_scope(void);
Sym *lookup_ordinary(Ident *name);
void bind_ordinary(Ident *name, Sym *sym);
int scope_level(void);
void make_static_data(Sym *var, Init *init);
Type *va_list_type(void);
Sym *func_name_sym(Token *tok);

// expr.c helpers
Expr *new_expr(ExprKind k, Type *ty, Token *tok);
Expr *cast_to(Expr *e, Type *ty);
Expr *assign_conv(Expr *e, Type *ty, Token *tok, const char *what);
Expr *rvalue(Expr *e);
Expr *new_iconst(int64_t v, Type *ty, Token *tok);
bool is_lvalue(Expr *e);
bool is_null_ptr_const(Expr *e);
bool eval_int_const(Expr *e, int64_t *out);
bool eval_static_addr(Expr *e, Sym **sym, int64_t *addend);
bool eval_float_const(Expr *e, double *out);
Expr *cond_expr(Expr *e);

//
// ir.c: an SSA intermediate representation
//

// Value types.  Integers narrower than 32 bits never appear as values:
// a `char` travels as an I32 that is already sign- or zero-extended.
// I8 and I16 exist only as memory types of loads and stores.
typedef enum {
  IR_VOID,
  IR_I8,
  IR_I16,
  IR_I32,
  IR_I64,
  IR_F32,
  IR_F64,
} IrType;

typedef enum {
  IR_NOP,
  IR_ICONST,  // ival
  IR_FCONST,  // fval
  IR_GADDR,   // address of `sym` + ival
  IR_ALLOCA,  // stack slot: ival bytes, align
  IR_PARAM,   // incoming argument register `ival` (class from ty)
  IR_ARGADDR, // address of the incoming stack-argument area + ival
  IR_COPY,
  IR_ADD,
  IR_SUB,
  IR_MUL,
  IR_SDIV,
  IR_UDIV,
  IR_SREM,
  IR_UREM,
  IR_AND,
  IR_OR,
  IR_XOR,
  IR_SHL,
  IR_LSHR,
  IR_ASHR,
  IR_NEG,
  IR_NOT,
  IR_FADD,
  IR_FSUB,
  IR_FMUL,
  IR_FDIV,
  IR_FNEG,
  IR_EQ, // integer comparisons produce an I32 0 or 1
  IR_NE,
  IR_SLT,
  IR_SLE,
  IR_SGT,
  IR_SGE,
  IR_ULT,
  IR_ULE,
  IR_UGT,
  IR_UGE,
  IR_FEQ,
  IR_FNE,
  IR_FLT,
  IR_FLE,
  IR_FGT,
  IR_FGE,
  IR_SEXT8,  // I32 -> I32: sign-extend the low 8 bits
  IR_SEXT16,
  IR_ZEXT8,
  IR_ZEXT16,
  IR_SEXT32, // I32 -> I64
  IR_ZEXT32, // I32 -> I64
  IR_TRUNC,  // I64 -> I32
  IR_SITOF,  // signed integer -> float
  IR_UITOF,
  IR_FTOSI,
  IR_FTOUI,
  IR_FEXT,   // F32 -> F64
  IR_FTRUNC, // F64 -> F32
  IR_BITCAST, // same-size reinterpretation between integer and float
  IR_LOAD,   // args[0] = address; memty, is_signed
  IR_STORE,  // args[0] = value, args[1] = address; memty
  IR_MEMCPY, // args[0] = dst, args[1] = src; ival bytes
  IR_MEMZERO, // args[0] = dst; ival bytes
  IR_CALL,
  IR_PHI,
  IR_JMP,
  IR_BR,     // args[0] != 0 ? blocks[0] : blocks[1]
  IR_SWITCH, // args[0] matched against case_vals[i] -> blocks[i+1], else blocks[0]
  IR_RET,
  IR_UNREACHABLE,
  IR_NUM_OPS,
} IrOp;

typedef enum { LOC_GPR, LOC_FPR, LOC_STACK } LocKind;

// Where one scalar piece of an argument travels.
typedef struct {
  char kind;  // LocKind
  char memty; // store type for LOC_STACK
  int reg;
  int off;
} ArgLoc;

typedef enum {
  RET_NONE,
  RET_SCALAR, // the call's own value, in x0 or v0
  RET_GPRS,   // x0 (and x1) stored into the buffer given as the last operand
  RET_FPRS,   // v0..v3 stored into the buffer given as the last operand
} RetKind;

typedef struct {
  ArgLoc *locs; // one per argument piece
  int nlocs;
  bool indirect; // args[0] is the callee address
  int stack_bytes;
  char ret_kind;
  char ret_elem; // IR type of each register for RET_GPRS / RET_FPRS
  int ret_n;
} CallInfo;

struct Inst {
  short op;
  char ty;
  char memty;
  bool is_signed;
  bool is_volatile;
  bool mark;
  int id;
  Inst *prev;
  Inst *next;
  Block *bb; // NULL for constants and symbol addresses (materialised at each use)
  Inst **args;
  int nargs;
  int64_t ival;
  double fval;
  Sym *sym;
  int align;
  Block **blocks; // successors of a terminator, incoming blocks of a phi
  int nblocks;
  int64_t *case_vals;
  CallInfo *call;
  Ident *var_name; // IR_ALLOCA: the variable it holds (for --dump-ir)

  // pass scratch
  Inst *fwd;      // replacement (set when the instruction is replaced)
  int nuses;
  Inst **users;
  int nusers;
  int lattice;    // SCCP
  int *arg_edge;  // SCCP: for each phi operand, the index of its edge into the block
  int64_t lat_i;
  double lat_f;
  int vreg;
  int aux;
  bool folded;    // isel: emitted as part of its user
};

struct Block {
  int id;
  Block *prev;
  Block *next; // layout order
  Inst *first;
  Inst *last;
  Block **preds;
  int npreds;
  int pred_cap;

  // dominators
  Block *idom;
  int rpo;
  int dom_pre;  // preorder number in the dominator tree
  int dom_post; // last preorder number in this block's dominator subtree
  Block **df;
  int ndf;
  Block **kids;
  int nkids;
  bool reachable;
  bool mark;
  int loop_depth;

  // machine code
  MInst *mfirst;
  MInst *mlast;
  int label;
  int mpos_start;
  int mpos_end;
  uint64_t *live_in;
  uint64_t *live_out;
};

struct Func {
  char *name;
  Sym *sym;
  Block *entry;
  Block *last;
  int nblocks;
  int next_inst_id;
  int next_block_id;
  int out_bytes;  // largest outgoing stack-argument area
  bool has_calls;
  bool no_promote; // calls setjmp: keep variables in memory

  // machine level
  int nvregs;
  int vreg_cap;
  char *vreg_cls; // 0 = general register, 1 = floating point
  int *vreg_hint;
  int *vreg_reg;  // physical register after allocation, or -1
  int *vreg_slot; // spill slot index, or -1
  int nslots;
  int nallocas; // slots 0..nallocas-1 hold IR allocas; the rest are spills
  int *slot_size;
  int *slot_align;
  int64_t *slot_off;
  uint32_t used_callee_gpr; // bit i: x(19+i)
  uint32_t used_callee_fpr; // bit i: d(8+i)
  int64_t frame_size;
  int64_t callee_area;
  int next_label;
  int id;
};

extern const char *ir_op_name[];
extern const char *ir_type_name[];

Func *ir_new_func(Sym *fn);
Block *ir_new_block(Func *f);
void ir_append_block(Func *f, Block *b);
void ir_insert_block_after(Func *f, Block *pos, Block *b);
void ir_remove_block(Func *f, Block *b);
Inst *ir_new(Func *f, int op, int ty, int nargs);
void ir_append(Block *b, Inst *i);
void ir_insert_before(Inst *pos, Inst *i);
void ir_insert_front(Block *b, Inst *i);
void ir_unlink(Inst *i);
Inst *ir_iconst(Func *f, int ty, int64_t v);
Inst *ir_fconst(Func *f, int ty, double v);
bool ir_is_terminator(int op);
bool ir_is_float(int ty);
bool ir_is_floating(Inst *i); // a constant or address with no block
bool ir_has_side_effects(Inst *i);
bool ir_is_pure(Inst *i);
int ir_size(int ty);
int ir_nsucc(Block *b);
Block *ir_succ(Block *b, int i);
void ir_compute_preds(Func *f);
int ir_pred_index(Block *b, Block *pred);
void ir_compute_uses(Func *f);
Inst *ir_resolve(Inst *i);
void ir_resolve_all(Func *f);
void ir_replace(Inst *old, Inst *with);
void ir_phi_remove_pred(Block *b, Block *pred);
void ir_phi_replace_pred(Block *b, Block *old, Block *with);
void ir_dump(FILE *out, Func *f);
void ir_verify(Func *f, const char *after);
void ir_renumber(Func *f);

//
// abi.c: Apple arm64 calling convention
//

typedef enum {
  AC_NONE,     // void or empty aggregate
  AC_INT,      // one general register
  AC_FP,       // one SIMD/FP register
  AC_INTS,     // aggregate of at most 16 bytes in 1-2 general registers
  AC_HFA,      // homogeneous floating-point aggregate in 1-4 FP registers
  AC_INDIRECT, // larger aggregate, passed by reference
} ArgClassKind;

typedef struct {
  int kind;
  int n;    // registers for AC_INTS / AC_HFA
  int elem; // IR_F32 or IR_F64 for AC_HFA / AC_FP
} ArgClass;

ArgClass abi_classify(Type *ty);
int ir_type_of(Type *ty);
int ir_memtype_of(Type *ty);

//
// irgen.c
//

Func *irgen_func(Sym *fn);
extern bool opt_dump_ir;
extern bool opt_dump_ir_raw;

//
// optimizer
//

extern int opt_level;
void optimize(Func *f);
void opt_simplify_cfg(Func *f);
void opt_split_critical_edges(Func *f);
void opt_dominators(Func *f);
bool opt_mem2reg(Func *f);
bool opt_sccp(Func *f);
bool opt_combine(Func *f);
bool opt_cse(Func *f);
bool opt_dce(Func *f);
void opt_remove_trivial_phis(Func *f);
void opt_loop_depth(Func *f);
bool dominates(Block *a, Block *b);
int64_t fold_int(int op, int ty, int64_t a, int64_t b, bool *ok);

//
// Machine level: isel.c, regalloc.c, emit.c
//

// Register numbers.  0-30 are x0-x30, 31 is sp (or zr where an
// instruction reads the zero register), 64-95 are v0-v31, and virtual
// registers start at 128.
enum {
  R_X0 = 0,
  R_X8 = 8,
  R_X15 = 15,
  R_X16 = 16,
  R_X17 = 17,
  R_X18 = 18,
  R_FP = 29,
  R_LR = 30,
  R_SP = 31,
  R_ZR = 32,
  R_V0 = 64,
  R_V29 = 93,
  R_V30 = 94,
  R_V31 = 95,
  R_VREG = 128,
  R_NONE = -1,
};

typedef enum {
  M_LABEL,    // block start
  M_MOV,      // rd = rn
  M_FMOV,     // rd = rn (floating)
  M_MOVI,     // rd = imm (any 64-bit value)
  M_FMOVI,    // rd = floating constant with bit pattern imm
  M_FMOV_GF,  // rd(fp) = bits of rn(gpr)
  M_FMOV_FG,  // rd(gpr) = bits of rn(fp)
  M_ADDR,     // rd = &sym + imm
  M_FRAME,    // rd = sp + offset of stack slot `slot` + imm
  M_ARGADDR,  // rd = x29 + 16 + imm
  M_ADD,      // rd = rn + (rm << shift)
  M_SUB,
  M_AND,
  M_ORR,
  M_EOR,
  M_ADDI,     // rd = rn + imm (imm may be negative)
  M_ANDI,     // logical immediates
  M_ORRI,
  M_EORI,
  M_MUL,
  M_MADD,     // rd = ra + rn * rm
  M_MSUB,     // rd = ra - rn * rm
  M_SDIV,
  M_UDIV,
  M_LSL,
  M_LSR,
  M_ASR,
  M_LSLI,
  M_LSRI,
  M_ASRI,
  M_NEG,
  M_MVN,
  M_CMP,      // flags = rn - rm
  M_CMPI,     // flags = rn - imm
  M_FCMP,
  M_FCMPZ,
  M_CSET,     // rd = cond ? 1 : 0
  M_SXTB,
  M_SXTH,
  M_SXTW,
  M_UXTB,
  M_UXTH,
  M_UXTW,     // rd(64) = zero-extended rn(32)
  M_LDR,      // rd = [base + imm]
  M_STR,      // [base + imm] = rn
  M_FADD,
  M_FSUB,
  M_FMUL,
  M_FDIV,
  M_FNEG,
  M_SCVTF,    // rd(fp sz) = rn(int sz2)
  M_UCVTF,
  M_FCVTZS,   // rd(int sz) = rn(fp sz2)
  M_FCVTZU,
  M_FCVT,     // rd(fp sz) = rn(fp sz2)
  M_BL,
  M_BLR,      // call x17 (set by the preceding parallel move)
  M_B,
  M_BCOND,
  M_CBZ,
  M_CBNZ,
  M_RET,      // epilogue + ret
  M_PMOVE,    // parallel move
  M_JTAB,     // jump table on rn (already range-checked and rebased)
  M_MEMCPY,   // inline copy of imm bytes from mem2 to mem (small sizes)
  M_MEMZERO,  // inline zeroing of imm bytes at mem
  M_TRAP,
  M_NUM_OPS,
} MOp;

typedef enum {
  C_EQ,
  C_NE,
  C_HS,
  C_LO,
  C_MI,
  C_PL,
  C_VS,
  C_VC,
  C_HI,
  C_LS,
  C_GE,
  C_LT,
  C_GT,
  C_LE,
} Cond;

// A memory operand: a register base, or a stack slot (resolved to an sp
// offset once the frame is laid out), plus a constant offset.
typedef struct {
  int base;  // register, or R_NONE when `slot` is used
  int slot;  // stack slot index, or -1
  int64_t off;
} MemRef;

typedef struct {
  int n;
  int *dst;      // physical or virtual register, or -1 when dst_slot is used
  int *src;      // register, or -1 when the source is the immediate
  int64_t *imm;
  char *cls;     // 0 = GPR, 1 = FPR
  char *sz;      // 4 or 8
} PMove;

struct MInst {
  short op;
  char sz;   // operand width in bytes: 4 or 8
  char sz2;  // source width for conversions
  char memty;
  bool is_signed;
  char cond;
  int rd;
  int rn;
  int rm;
  int ra;
  int shift;
  int64_t imm;
  Sym *sym;
  MemRef mem;
  MemRef mem2; // M_MEMCPY: the source
  Block *target;
  PMove *pm;
  Block **jt;
  int njt;
  int pos; // linear position, for register allocation
  MInst *prev;
  MInst *next;
};

void isel_func(Func *f);
void regalloc_func(Func *f);
void emit_begin(FILE *out, const char *src_name);
void emit_func(Func *f);
void emit_data(Program *prog);
void emit_end(void);
bool encode_logical_imm(uint64_t v, int sz, uint32_t *enc);
bool fmov_imm_ok(double v, int sz);
int minst_regs(MInst *mi, int *uses, int *nuses, int *defs, int *ndefs);
bool is_vreg(int r);
int new_vreg(Func *f, int cls);
void mir_dump(FILE *out, Func *f);
extern bool opt_dump_mir;

//
// main.c
//

extern char *opt_base_file;

#endif
