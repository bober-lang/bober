#include <stdint.h>

typedef struct Sym Sym;
typedef struct Node Node;
typedef struct Str Str;
typedef struct Arr Arr;
typedef struct Obj Obj;
typedef struct Fn Fn;
typedef struct Type Type;
typedef struct Global Global;
typedef struct Mod Mod;

enum { K_NULL, K_BOOL, K_I8, K_I16, K_I32, K_I64, K_U8, K_U16, K_U32, K_U64, K_INT, K_F32, K_F64, K_FLT,
       K_STR, K_ARR, K_OBJ, K_FN, K_TYPE, K_MOD };

typedef struct { int k; union { int64_t i; double f; Str *s; Arr *a; Obj *o; Fn *fn; Type *t; Mod *m; }; } Val;

struct Sym { Sym *next; int tok, len; char s[]; };

enum {
  N_LIT, N_TPL, N_LOCAL, N_GLOBAL, N_THIS, N_ARRAY, N_OBJECT, N_PROP, N_SPREAD, N_FN, N_PARAM,
  N_UNARY, N_BIN, N_CMP, N_AND, N_OR, N_QQ, N_IS, N_AS, N_IFX, N_SWITCHX, N_CASE, N_RANGE,
  N_CALL, N_MEMBER, N_INDEX, N_NONNULL, N_NEW,
  N_BLOCK, N_VAR, N_CLASS, N_IFACE, N_TYPEDEF, N_IF, N_FOR, N_WHILE, N_SWITCH, N_TRY, N_CATCH,
  N_RETURN, N_THROW, N_BREAK, N_CONTINUE, N_TEST, N_ASSERT, N_ASSIGN, N_IMPORT,
  P_ID, P_ARR, P_OBJ, P_ELEM
};

enum { F_OPT = 1, F_PAREN = 2, F_CHAIN = 4, F_REST = 8, F_SPREAD = 16, F_STATIC = 32, F_HEAP = 64,
       F_METHOD = 128, F_DYN = 256, F_EMBED = 512, F_EXTERN = 1024 };

struct Node {
  int k, op, flags, line, col, n, slot, depth, nslots, ntp, cidx;
  const char *file;
  Node *a, *b, *c, *d, **v, **tdefs;
  Sym *sym, *as, **names;
  Val val;
  Type *tc;
  Global *g;
  Mod *mod, *cmod;
};

typedef struct { int t, line, col, op; Sym *s; Node *n; } Tok;

extern Tok *toks;
extern int ntoks;
extern const char *curfile;
extern Type *anytype;

void *alloc(long n);
Sym *intern(const char *s, int len);
Str *mkstr(const char *p, int len);
void kwinit(void);
void lexall(const char *src, long len);
Node *parse(const char *file, const char *src, long len, Mod *m, int header);
