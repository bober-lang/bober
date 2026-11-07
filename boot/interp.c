#include <alloca.h>
#include <dlfcn.h>
#if __has_include(<ffi/ffi.h>)
#include <ffi/ffi.h>
#else
#include <ffi.h>
#endif
#include <limits.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <math.h>
#include <pthread.h>
#define GC_THREADS  // so that pthread_create registers the thread with the collector
#include <gc.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ast.h"
#include "parse.h"

typedef struct Buf { int used, cap; char d[]; } Buf;
typedef struct Frame Frame;
typedef struct Class Class;
typedef Val Builtin(Val self, Val *args, int n, Node *site);

struct Str { int len; char *p; Buf *buf; };
struct Arr { int len, cap, lit; Val *v; Type *elem; };
struct Obj { Class *cls; Type *type; int n, lit; Sym **names; Val *v; };
struct Frame { Frame *up; Val this; Val s[]; };
struct Fn { Node *def; Frame *env; Val self; Val *targs; int nt; Builtin *bi; void *sym; ffi_cif *cif; };
struct Type { int tk, prim, n; Type *elem, **mem, *base; Sym **names; Class *cls; Val lit; Arr *members, *args; };
struct Global { Sym *name; Val v; int state; Node *def; Frame *env; };
struct Mod { char *path; int n; Sym **names; Global **g; Node *prog; Mod *next; };
struct Class { Node *def; Frame *frame; Fn *ctor; Type *type; int nf; Sym **fnames; Node **fprops; Mod methods, statics;
  Class *generic, *instances, *next; };

enum { T_ANY, T_PRIM, T_LIT, T_ARR, T_TUP, T_UNION, T_OBJ, T_CLASS, T_FN };
enum { S_NORMAL, S_RET, S_BRK, S_CONT };
enum { UNINIT, BUSY, DONE };

typedef struct Handler { struct Handler *up; jmp_buf jb; int depth; } Handler;

Type *anytype;
static Type *prims[K_STR + 1], *fntype, anyarr = {T_ARR}, anyobj = {T_OBJ};
static Handler *handlers;
static jmp_buf trapjb;
static Str *testpath;
static int subfails;
static char trapmsg[1024];
static Val thrown, retval;
static Node *thrownat;
static int depth, argc_;
static char **argv_;
static Mod *mods, *prelude, *strmethods, *arrmethods, *mem;
static Sym *s_length, *s_toString, *s_constructor, *s_main, *s_members, *s_value, *s_kind, *s_name, *s_fields, *s_elem, *s_tags, *s_base, *s_args, *s_field[4];
static const char *primnames[] = {"null", "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "", "f32", "f64", "",
  "string"};

#define NUL ((Val){K_NULL})
#define B(x) ((Val){K_BOOL, .i = !!(x)})
#define I(k, x) ((Val){k, .i = (x)})
#define F(k, x) ((Val){k, .f = (x)})
#define S(x) ((Val){K_STR, .s = (x)})
#define A(x) ((Val){K_ARR, .a = (x)})
#define O(x) ((Val){K_OBJ, .o = (x)})
#define FN(x) ((Val){K_FN, .fn = (x)})
#define TY(x) ((Val){K_TYPE, .t = (x)})
#define FRAME(d) ((d)->flags & F_HEAP ? alloc(fsize(d)) : ({ Frame *f_ = alloca(fsize(d)); memset(f_, 0, fsize(d)); }))

static Val eval(Node *n, Frame *f);
static int exec(Node *n, Frame *f);
static Val call(Fn *fn, Val self, Val *args, int na, Node *site);
static Class *instance(Class *c, Node *n, Frame *f);
static Class *origin(Class *c) { return c->generic ? c->generic : c; }
static Type *evaltype(Node *n, Frame *f);
static Type *typeofv(Val v);
static Val conv(Type *t, Val v);
static Str *tostr(Val v);
static Str *cat(Str *a, Str *b);
static Val decl(Node *d, Frame *f);

// Zeroed memory from Boehm's collector, which finds pointers to it in stacks, globals and collected memory, not in malloc's.
void *alloc(long n) { return GC_MALLOC(n); }

Sym *intern(const char *s, int len) {
  static Sym *tab[4096];
  unsigned h = 5381;
  for (int i = 0; i < len; i++) h = h * 33 + (unsigned char)s[i];
  Sym **p = &tab[h & 4095];
  for (Sym *x = *p; x; x = x->next) if (x->len == len && !memcmp(x->s, s, len)) return x;
  Sym *x = alloc(sizeof(Sym) + len + 1);
  memcpy(x->s, s, len);
  x->len = len, x->next = *p;
  return *p = x;
}

Str *mkstr(const char *p, int len) {
  Str *s = alloc(sizeof(Str) + len);
  s->len = len, s->p = memcpy((char *)(s + 1), p, len);
  return s;
}

_Noreturn static void trap(Node *n, const char *fmt, ...) {
  int k = snprintf(trapmsg, sizeof trapmsg, "%s:%d:%d: trap: ", n->file, n->line, n->col);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(trapmsg + k, sizeof trapmsg - k, fmt, ap);
  va_end(ap);
  longjmp(trapjb, 1);
}

static Str *tstr(const char *s) { return mkstr(s, strlen(s)); }

static Str *shown(Val v) {
  if (v.k != K_OBJ) return tostr(v);
  Obj *o = v.o;
  Str *s = o->cls ? mkstr(o->cls->def->sym->s, o->cls->def->sym->len) : tstr("");
  for (int i = 0; i < o->n; i++) {
    s = cat(cat(s, tstr(i ? ", " : " { ")), mkstr(o->names[i]->s, o->names[i]->len));
    s = cat(cat(s, tstr(": ")), tostr(o->v[i]));
  }
  return o->n ? cat(s, tstr(" }")) : s;
}

_Noreturn static void throwv(Val v) {
  if (!handlers) {
    Str *s = shown(v);
    trap(thrownat, "uncaught throw: %.*s", s->len, s->p);
  }
  thrown = v;
  longjmp(handlers->jb, 1);
}

static int fsize(Node *d) { return sizeof(Frame) + d->nslots * sizeof(Val); }
static int isint(int k) { return k >= K_I8 && k <= K_INT; }
static int isflt(int k) { return k >= K_F32 && k <= K_FLT; }
static int isnum(int k) { return k >= K_I8 && k <= K_FLT; }
static int isuns(int k) { return k >= K_U8 && k <= K_U64; }
static int truth(Val v) { return v.i != 0; }
static const int bits[] = {[K_I8] = 8, 16, 32, 64, 8, 16, 32, 64, 64};

static int64_t wrap(int k, int64_t x) {
  int b = 64 - bits[k];
  return isuns(k) ? (int64_t)((uint64_t)x << b >> b) : (int64_t)((uint64_t)x << b) >> b;
}

static double fval(Val v) { return isflt(v.k) ? v.f : isuns(v.k) ? (double)(uint64_t)v.i : (double)v.i; }

static Val cast(int k, Val v) {
  if (isflt(k)) return F(k, k == K_F32 ? (float)fval(v) : fval(v));
  if (!isflt(v.k)) return I(k, wrap(k, v.i));
  double d = v.f, lim = ldexp(1, isuns(k) ? bits[k] : bits[k] - 1);
  if (d != d) return I(k, 0);
  if (isuns(k)) return I(k, d <= 0 ? 0 : d >= lim ? wrap(k, -1) : (int64_t)(uint64_t)d);
  int64_t max = (int64_t)((1ULL << (bits[k] - 1)) - 1);
  return I(k, d <= -lim ? -max - 1 : d >= lim ? max : (int64_t)d);
}

static int join(int a, int b) {
  if (a == b) return a;
  if (a == K_INT) return b;
  if (b == K_INT) return a;
  if (isflt(a) || isflt(b)) return a == K_FLT && isflt(b) ? b : b == K_FLT && isflt(a) ? a : K_F64;
  return bits[a] >= bits[b] ? a : b;
}

static Str *cat(Str *a, Str *b) {
  Buf *u = a->buf;
  char *p = a->p;
  if (!u || p + a->len != u->d + u->used || u->used + b->len > u->cap) {
    int cap = 2 * (a->len + b->len) + 16;
    u = alloc(sizeof(Buf) + cap);
    u->cap = cap, u->used = a->len;
    p = memcpy(u->d, a->p, a->len);
  }
  memcpy(u->d + u->used, b->p, b->len);
  u->used += b->len;
  Str *s = alloc(sizeof(Str));
  s->len = a->len + b->len, s->p = p, s->buf = u;
  return s;
}

static Str *tostr(Val v) {
  static const char *names[] = {"null", "false", [K_ARR] = "<array>", "<object>", "<function>", "<type>", "<module>"};
  char b[64];
  if (v.k == K_STR) return v.s;
  if (v.k == K_BOOL && v.i) return mkstr("true", 4);
  if (isuns(v.k)) snprintf(b, sizeof b, "%llu", (unsigned long long)v.i);
  else if (isint(v.k)) snprintf(b, sizeof b, "%lld", (long long)v.i);
  else if (isflt(v.k))
    for (int p = 1; p <= 17; p++) {
      snprintf(b, sizeof b, "%.*g", p, v.f);
      if (v.k == K_F32 ? strtof(b, 0) == (float)v.f : strtod(b, 0) == v.f) break;
    }
  else return mkstr(names[v.k], strlen(names[v.k]));
  return mkstr(b, strlen(b));
}

/* Types are equal by structure where arrays, as string[] makes a new one each time. */
static int sametype(Type *a, Type *b) {
  if (a == b) return 1;
  if (a->tk != b->tk || a->n != b->n) return 0;
  if (a->tk == T_PRIM) return a->prim == b->prim;
  return a->tk == T_ARR && a->elem && b->elem && sametype(a->elem, b->elem);
}

static int eq(Val a, Val b) {
  if (isnum(a.k) && isnum(b.k)) {
    int k = join(a.k, b.k);
    return isflt(k) ? cast(k, a).f == cast(k, b).f : cast(k, a).i == cast(k, b).i;
  }
  if (a.k != b.k) return 0;
  if (a.k == K_STR) return a.s->len == b.s->len && !memcmp(a.s->p, b.s->p, a.s->len);
  if (a.k == K_TYPE) return sametype(a.t, b.t);
  return a.k == K_NULL || a.i == b.i;
}

static int relop(int op, Val a, Val b, Node *n) {
  if (op == EQ || op == NE) return eq(a, b) == (op == EQ);
  int c;
  if (a.k == K_STR && b.k == K_STR) {
    int m = a.s->len < b.s->len ? a.s->len : b.s->len;
    c = memcmp(a.s->p, b.s->p, m);
    if (!c) c = a.s->len - b.s->len;
  } else {
    if (!isnum(a.k) || !isnum(b.k)) trap(n, "cannot compare");
    int k = join(a.k, b.k);
    if (isflt(k)) {
      double x = cast(k, a).f, y = cast(k, b).f;
      return op == '<' ? x < y : op == '>' ? x > y : op == LE ? x <= y : x >= y;
    }
    uint64_t x = cast(k, a).i, y = cast(k, b).i;
    c = isuns(k) ? (x > y) - (x < y) : ((int64_t)x > (int64_t)y) - ((int64_t)x < (int64_t)y);
  }
  return op == '<' ? c < 0 : op == '>' ? c > 0 : op == LE ? c <= 0 : c >= 0;
}

static Val arith(int op, Val a, Val b, Node *n) {
  if (op == '+' && (a.k == K_STR || b.k == K_STR)) return S(cat(tostr(a), tostr(b)));
  if (!isnum(a.k) || !isnum(b.k)) trap(n, "bad operands");
  int k = join(a.k, b.k);
  if (isflt(k)) {
    double x = cast(k, a).f, y = cast(k, b).f, r;
    switch (op) {
    case '+': r = x + y; break;
    case '-': r = x - y; break;
    case '*': r = x * y; break;
    case '/': r = x / y; break;
    case '%': r = fmod(x, y); break;
    case POW: r = pow(x, y); break;
    default: trap(n, "bad operands");
    }
    return F(k, k == K_F32 ? (float)r : r);
  }
  uint64_t x = cast(k, a).i, y = cast(k, b).i, r = 0, w = bits[k] - 1;
  int u = isuns(k);
  switch (op) {
  case '+': r = x + y; break;
  case '-': r = x - y; break;
  case '*': r = x * y; break;
  case '&': r = x & y; break;
  case '|': r = x | y; break;
  case '^': r = x ^ y; break;
  case SHL: r = x << (y & w); break;
  case SHR: r = u ? x >> (y & w) : (uint64_t)((int64_t)x >> (y & w)); break;
  case '/':
  case '%':
    if (!y) trap(n, "division by zero");
    if (u) r = op == '/' ? x / y : x % y;
    else if ((int64_t)y == -1) r = op == '/' ? -x : 0;
    else r = op == '/' ? (uint64_t)((int64_t)x / (int64_t)y) : (uint64_t)((int64_t)x % (int64_t)y);
    break;
  case POW:
    if (!u && (int64_t)y < 0) trap(n, "negative exponent");
    for (r = 1; y; y >>= 1, x *= x) if (y & 1) r *= x;
    break;
  }
  return I(k, wrap(k, r));
}

static Type *newtype(int tk, int n) {
  Type *t = alloc(sizeof(Type));
  t->tk = tk, t->n = n;
  if (n > 0 && tk != T_ARR) t->mem = alloc(n * sizeof(Type *));
  return t;
}

static Type *totype(Val v) {
  Type *t;
  switch (v.k) {
  case K_TYPE: return v.t;
  case K_NULL: return prims[K_NULL];
  case K_FN: return fntype;
  case K_ARR:
    t = newtype(T_TUP, v.a->len);
    for (int i = 0; i < t->n; i++) t->mem[i] = totype(v.a->v[i]);
    return t;
  case K_OBJ:
    t = newtype(T_OBJ, v.o->n);
    t->names = v.o->names;
    for (int i = 0; i < t->n; i++) t->mem[i] = totype(v.o->v[i]);
    return t;
  }
  t = newtype(T_LIT, 0);
  t->lit = v;
  return t;
}

static Type *unite(Type *a, Type *b) {
  int na = a->tk == T_UNION ? a->n : 1, nb = b->tk == T_UNION ? b->n : 1;
  Type *t = newtype(T_UNION, na + nb);
  for (int i = 0; i < na; i++) t->mem[i] = a->tk == T_UNION ? a->mem[i] : a;
  for (int i = 0; i < nb; i++) t->mem[na + i] = b->tk == T_UNION ? b->mem[i] : b;
  return t;
}

static int dyn(Node *n) {
  if (!n || n->k == N_LOCAL || n->k == N_THIS) return n != 0;
  int d = dyn(n->a) || dyn(n->b) || dyn(n->c) || dyn(n->d);
  for (int i = 0; i < n->n && !d; i++) d = dyn(n->v[i]);
  return d;
}

static Type *evaltype(Node *n, Frame *f) {
  if (n->tc) return n->tc;
  // In a type, | unites, also integer consts, which are literal types there.
  Type *t = n->k == N_BIN && n->op == '|' ? unite(evaltype(n->a, f), evaltype(n->b, f)) : totype(eval(n, f));
  if (!(n->flags & F_DYN) && !dyn(n)) n->tc = t;
  n->flags |= F_DYN;
  return t;
}

static int field(Obj *o, Sym *s, Node *cache) {
  if (cache && o->n && cache->names == o->names) return cache->cidx;
  int i = o->n;
  while (i-- > 0 && o->names[i] != s);
  if (cache) cache->names = o->names, cache->cidx = i;
  return i;
}

static Global *lookup(Mod *m, Sym *s) {
  for (int i = 0; i < m->n; i++) if (m->names[i] == s) return m->g[i];
  return 0;
}

static void add(Mod *m, Sym *s, Global *g) {
  if (!(m->n % 64)) {
    m->names = memcpy(alloc((m->n + 64) * sizeof(Sym *)), m->names, m->n * sizeof(Sym *));
    m->g = memcpy(alloc((m->n + 64) * sizeof(Global *)), m->g, m->n * sizeof(Global *));
  }
  m->names[m->n] = s, m->g[m->n++] = g;
}

static Global *global(Mod *m, Sym *s, Node *def, Val v) {
  Global *g = alloc(sizeof(Global));
  g->name = s, g->def = def, g->v = v, g->state = def ? UNINIT : DONE;
  add(m, s, g);
  return g;
}

static Global *find(Mod *m, Sym *s, Node *cache) {
  if (!cache) return lookup(m, s);
  if (cache->cmod != m) cache->cmod = m, cache->g = lookup(m, s);
  return cache->g;
}

static Fn *method(Class *c, Sym *s, Node *cache) {
  Global *g = c ? find(&c->methods, s, cache) : 0;
  return g ? g->v.fn : 0;
}

static Type *fieldtype(Type *t, Sym *s) {
  for (int i = 0; i < t->n; i++) if (t->names[i] == s) return t->mem[i];
  return 0;
}

static int is(Val v, Type *t) {
  switch (t->tk) {
  case T_ANY: return 1;
  case T_PRIM: return v.k == t->prim || (v.k == K_INT && isint(t->prim)) || (v.k == K_FLT && isflt(t->prim));
  case T_LIT: return eq(v, t->lit);
  case T_ARR:
  case T_TUP: return v.k == K_ARR;
  case T_FN: return v.k == K_FN;
  case T_CLASS: return v.k == K_OBJ && origin(v.o->cls) == origin(t->cls);
  case T_UNION:
    for (int i = 0; i < t->n; i++) if (is(v, t->mem[i])) return 1;
    return 0;
  case T_OBJ:
    if (v.k != K_OBJ) return 0;
    for (int i = 0; i < t->n; i++) {
      int k = t->names[i] ? field(v.o, t->names[i], 0) : 0;
      if (k < 0 ? !method(v.o->cls, t->names[i], 0) : t->mem[i] && t->mem[i]->tk == T_LIT && !eq(v.o->v[k], t->mem[i]->lit))
        return 0;
    }
    return 1;
  }
  return 0;
}

static Arr *mkarr(int len) {
  Arr *a = alloc(sizeof(Arr));
  a->len = a->cap = len, a->v = alloc(len * sizeof(Val));
  return a;
}

static Obj *mkobj(int n, Sym **names) {
  Obj *o = alloc(sizeof(Obj));
  o->n = n, o->names = names, o->v = alloc(n * sizeof(Val));
  return o;
}

static Val deflt(Val v) {
  if (v.k == K_INT) return cast(K_I32, v);
  if (v.k == K_FLT) return F(K_F64, v.f);
  return v.k == K_ARR ? conv(&anyarr, v) : v.k == K_OBJ ? conv(&anyobj, v) : v;
}

static Type *pick(Type *t, Val v) {
  Type *num = 0, *flt = 0;
  for (int i = 0; i < t->n; i++) {
    Type *m = t->mem[i];
    if (m->tk == T_LIT && isint(m->lit.k) && isint(v.k)) m = typeofv(m->lit); // stored as its base
    if (m->tk == T_PRIM ? m->prim == v.k : m->tk != T_LIT && is(v, m)) return m;
    if (m->tk != T_PRIM || !isnum(v.k) || !isnum(m->prim)) continue;
    if (!num && isflt(m->prim) == isflt(v.k)) num = m;
    if (!flt && v.k == K_INT && isflt(m->prim)) flt = m;
  }
  return num ? num : flt;
}

static Val conv(Type *t, Val v) {
  switch (t ? t->tk : T_ANY) {
  case T_PRIM: return isnum(t->prim) && isnum(v.k) ? cast(t->prim, v) : v;
  case T_UNION: return (t = pick(t, v)) ? conv(t, v) : deflt(v);
  case T_ARR:
  case T_TUP:
    if (v.k != K_ARR || !v.a->lit) return v;
    v.a->lit = 0, v.a->elem = t->tk == T_ARR ? t->elem : 0;
    for (int i = 0; i < v.a->len; i++)
      v.a->v[i] = conv(t->tk == T_ARR ? t->elem : i < t->n ? t->mem[i] : 0, v.a->v[i]);
    return v;
  case T_OBJ:
    if (v.k != K_OBJ || !v.o->lit) return v;
    v.o->lit = 0, v.o->type = t;
    for (int i = 0; i < v.o->n; i++) v.o->v[i] = conv(fieldtype(t, v.o->names[i]), v.o->v[i]);
    return v;
  }
  return deflt(v);
}

static Val store(Type *t, Val old, Val v) {
  return t ? conv(t, v) : isnum(old.k) && isnum(v.k) ? cast(old.k, v) : deflt(v);
}

static Val codepoint(int k, Str *s, Node *n) {
  unsigned char *p = (unsigned char *)s->p;
  static const int64_t least[] = {0, 0, 0x80, 0x800, 0x10000};
  int len = !s->len ? 0 : p[0] < 0x80 ? 1 : p[0] < 0xC0 ? 0 : p[0] < 0xE0 ? 2 : p[0] < 0xF0 ? 3 : 4;
  if (!len || len != s->len) trap(n, "not one character");
  int64_t c = p[0] & 0xFF >> len;
  for (int i = 1; i < len; i++)
    if ((p[i] & 0xC0) != 0x80) trap(n, "not one character");
    else c = c << 6 | (p[i] & 63);
  if (c < least[len] || c > 0x10FFFF) trap(n, "not one character");
  if (cast(k, I(K_I64, c)).i != c || (k == K_U8 && c > 0x7F)) trap(n, "not representable");
  return I(k, c);
}

static Val zero(Type *t) {
  Arr *a;
  switch (t ? t->tk : T_ANY) {
  case T_PRIM: return t->prim == K_STR ? S(mkstr("", 0)) : isflt(t->prim) ? F(t->prim, 0) : (Val){t->prim};
  case T_LIT: return t->lit;
  case T_UNION:
    for (int i = 0; i < t->n; i++) if (t->mem[i] == prims[K_NULL]) return NUL;
    return zero(t->mem[0]);
  case T_ARR:
  case T_TUP:
    a = mkarr(t->n > 0 ? t->n : 0);
    a->elem = t->tk == T_ARR ? t->elem : 0;
    for (int i = 0; i < a->len; i++) a->v[i] = zero(t->tk == T_ARR ? t->elem : t->mem[i]);
    return A(a);
  }
  return NUL;
}

static Fn *closure(Node *d, Frame *env) {
  Fn *fn = alloc(sizeof(Fn));
  fn->def = d, fn->env = env;
  return fn;
}

static Fn *bound(Fn *m, Val self) {
  Fn *fn = alloc(sizeof(Fn));
  *fn = *m;
  fn->self = self;
  return fn;
}

static void define(Val *cell, Node *d, Frame *f) {
  if (d->k != N_TYPEDEF && d->k != N_IFACE) {
    *cell = decl(d, f);
    return;
  }
  *cell = TY(newtype(T_ANY, 0));
  Type *t = decl(d, f).t;
  *cell->t = *t;
}

static Val gval(Global *g) {
  if (g->state == DONE) return g->v;
  if (g->state == BUSY && g->def->k != N_TYPEDEF && g->def->k != N_IFACE)
    trap(g->def, "cycle in initialization of %s", g->name->s);
  if (g->state == BUSY) return g->v;
  Handler *h = handlers;
  handlers = 0;
  g->state = BUSY;
  define(&g->v, g->def, g->env);
  g->state = DONE;
  handlers = h;
  return g->v;
}

static Global *gref(Node *n) {
  if (!n->g && !(n->g = lookup(n->mod, n->sym)) && !(n->g = lookup(prelude, n->sym)))
    trap(n, "undefined name %s", n->sym->s);
  return n->g;
}

static Val *local(Node *n, Frame *f) {
  for (int d = n->depth; d; d--) f = f->up;
  return &f->s[n->slot];
}

static Type *typeofv(Val v) {
  Type *t;
  switch (v.k) {
  case K_INT: return prims[K_I32];
  case K_FLT: return prims[K_F64];
  case K_FN: return fntype;
  case K_ARR:
    t = newtype(T_ARR, -1);
    t->elem = v.a->elem ? v.a->elem : anytype;
    return t;
  case K_OBJ:
    if (v.o->cls) return v.o->cls->type;
    t = newtype(T_OBJ, v.o->n);
    t->names = v.o->names;
    for (int i = 0; i < t->n; i++) t->mem[i] = typeofv(v.o->v[i]);
    return t;
  }
  return v.k <= K_STR ? prims[v.k] : anytype;
}

static const char *kind(Type *t) {
  static const char *kinds[] = {[T_ANY] = "type", [T_LIT] = "literal", [T_ARR] = "array", [T_TUP] = "tuple",
    [T_UNION] = "union", [T_OBJ] = "object", [T_CLASS] = "class", [T_FN] = "function"};
  if (t->tk == T_ARR && t->n >= 0) return "fixed";
  if (t->tk != T_PRIM) return kinds[t->tk];
  int k = t->prim;
  return k == K_NULL || k == K_BOOL || k == K_STR ? primnames[k] : isint(k) ? "int" : "float";
}

static Str *typename(Type *t) {
  if (t->tk == T_CLASS) return mkstr(t->cls->def->sym->s, t->cls->def->sym->len);
  return tstr(t->tk == T_PRIM ? primnames[t->prim] : kind(t));
}

static int nfields(Type *t) { return t->tk == T_CLASS ? t->cls->nf : t->n; }

/// The tags l of a class or field as Types, an instantiated generic one with its base and args.
static Arr *tags(Node *l, Frame *f) {
  Arr *a = mkarr(l ? l->n : 0);
  for (int i = 0; i < a->len; i++) {
    Node *n = l->v[i];
    Type *t = evaltype(n, f);
    if (n->k == N_INDEX && t->tk == T_CLASS) {
      Type *u = newtype(T_CLASS, 0);
      *u = *t, u->base = origin(t->cls)->type, u->args = mkarr(n->n);
      for (int j = 0; j < n->n; j++) u->args->v[j] = TY(evaltype(n->v[j], f));
      t = u;
    }
    a->v[i] = TY(t);
  }
  return a;
}

static Arr *fields(Type *t, Node *site) {
  if (t->tk != T_CLASS && t->tk != T_OBJ && t->tk != T_TUP) trap(site, "no fields");
  int n = nfields(t);
  Arr *a = mkarr(n);
  for (int i = 0; i < n; i++) {
    Obj *o = mkobj(4, s_field);
    Sym *s = t->tk == T_CLASS ? t->cls->fnames[i] : t->names ? t->names[i] : 0;
    Node *p = t->tk == T_CLASS ? t->cls->fprops[i] : 0;
    char b[16];
    o->v[0] = S(s ? mkstr(s->s, s->len) : mkstr(b, snprintf(b, sizeof b, "%d", i)));
    o->v[1] = TY(!p ? t->mem[i] : p->a ? evaltype(p->a, t->cls->frame) : anytype);
    o->v[2] = B(0);
    o->v[3] = A(p ? tags(p->d, t->cls->frame) : mkarr(0));
    a->v[i] = O(o);
  }
  return a;
}

static Val member(Val o, Node *n, int bind) {
  Sym *s = n->sym;
  Global *g = 0;
  Fn *m = 0;
  int i;
  switch (o.k) {
  case K_OBJ:
    if ((i = field(o.o, s, n)) >= 0) return o.o->v[i];
    m = method(o.o->cls, s, n);
    break;
  case K_STR:
  case K_ARR:
    if (s == s_length) return I(K_I32, o.k == K_STR ? o.s->len : o.a->len);
    if ((g = find(o.k == K_STR ? strmethods : arrmethods, s, n))) return FN(bind ? bound(g->v.fn, o) : g->v.fn);
    break;
  case K_TYPE:
    if (o.t->tk == T_CLASS && (g = find(&o.t->cls->statics, s, n))) break;
    if (s == s_kind) return S(tstr(kind(o.t)));
    if (s == s_name) return S(typename(o.t));
    if (s == s_fields) return A(fields(o.t, n));
    if (s == s_tags) return A(o.t->tk == T_CLASS ? tags(o.t->cls->def->d, o.t->cls->frame) : mkarr(0));
    if (s == s_args) return A(o.t->args ? o.t->args : mkarr(0));
    if (s == s_base) return TY(o.t->base ? o.t->base : o.t);
    if (o.t->tk == T_ARR && s == s_elem) return TY(o.t->elem ? o.t->elem : anytype);
    if (o.t->tk == T_ARR && o.t->n >= 0 && s == s_length) return I(K_I32, o.t->n);
    if (o.t->tk == T_LIT && s == s_value) return o.t->lit;
    if (o.t->tk == T_UNION && s == s_members) {
      if (!o.t->members) {
        o.t->members = mkarr(o.t->n);
        for (int i = 0; i < o.t->n; i++) o.t->members->v[i] = TY(o.t->mem[i]);
      }
      return A(o.t->members);
    }
    break;
  case K_MOD: g = find(o.m, s, n); break;
  }
  if (g) return gval(g);
  if (m) return FN(bind ? bound(m, o) : m);
  trap(n, "no member %s", s->s);
}

static int idx(Val i, int len, Node *n) {
  if (i.k == K_STR) { // a tuple's field by the name fields gives it, its index
    int k = 0;
    for (int j = 0; j < i.s->len; j++) k = k * 10 + i.s->p[j] - '0';
    i = I(K_I32, k);
  }
  if (!isint(i.k) || i.i < 0 || i.i >= len) trap(n, "index out of range");
  return i.i;
}

static Val subscript(Val o, Node *n, Frame *f) {
  if (o.k == K_FN && n->n) {
    Fn *fn = bound(o.fn, o.fn->self);
    fn->nt = n->n, fn->targs = alloc(n->n * sizeof(Val));
    for (int i = 0; i < n->n; i++) fn->targs[i] = TY(evaltype(n->v[i], f));
    return FN(fn);
  }
  Val i = n->n ? eval(n->v[0], f) : I(K_I32, -1);
  if (n->n && o.k == K_ARR) return o.a->v[idx(i, o.a->len, n)];
  if (n->n && o.k == K_OBJ && i.k == K_STR) {
    int k = field(o.o, intern(i.s->p, i.s->len), 0);
    if (k < 0) trap(n, "no field %.*s", i.s->len, i.s->p);
    return o.o->v[k];
  }
  if (n->n && o.k == K_STR) return I(K_U8, (uint8_t)o.s->p[idx(i, o.s->len, n)]);
  if (!n->n || (o.k == K_TYPE && isint(i.k))) {
    Type *t = newtype(T_ARR, i.i);
    t->elem = totype(o);
    return TY(t);
  }
  if (o.k == K_TYPE && o.t->tk == T_CLASS && o.t->cls->def->ntp) return TY(instance(o.t->cls, n, f)->type);
  if (o.k == K_TYPE) return o;
  trap(n, "cannot subscript");
}

/// Where field k of o is, of type t, embedded or not.
static Val *fref(Obj *o, int k, Type **t, int *embed) {
  Class *c = o->cls;
  *embed = c && c->fprops[k]->flags & F_EMBED;
  *t = c ? (c->fprops[k]->a ? evaltype(c->fprops[k]->a, c->frame) : 0) : o->type ? fieldtype(o->type, o->names[k]) : 0;
  return &o->v[k];
}

/// Where target n is, of type t, embedded or not, and in an array's elements in if there.
static Val *lref(Node *n, Frame *f, Type **t, int *embed, Arr **in) {
  Val o, i;
  Global *g = 0;
  int k;
  *t = 0, *embed = 0, *in = 0;
  switch (n->k) {
  case N_LOCAL:
    for (int d = n->depth; d; d--) f = f->up;
    if (n->a) *t = evaltype(n->a, f);
    return &f->s[n->slot];
  case N_GLOBAL: g = gref(n); break;
  case N_MEMBER:
    o = eval(n->a, f);
    if (o.k == K_OBJ && (k = field(o.o, n->sym, n)) >= 0) return fref(o.o, k, t, embed);
    if (o.k == K_TYPE && o.t->tk == T_CLASS) g = find(&o.t->cls->statics, n->sym, n);
    break;
  case N_INDEX:
    o = eval(n->a, f), i = eval(n->v[0], f);
    if (o.k == K_ARR) return *t = o.a->elem, *in = o.a, &o.a->v[idx(i, o.a->len, n)];
    if (o.k == K_OBJ && i.k == K_STR && (k = field(o.o, intern(i.s->p, i.s->len), 0)) >= 0) return fref(o.o, k, t, embed);
    break;
  }
  if (!g && n->k == N_MEMBER) trap(n, "cannot assign %.*s, no such member", n->sym->len, n->sym->s);
  if (!g) trap(n, "cannot assign");
  gval(g);
  if ((g->def->k == N_VAR || g->def->k == N_PROP) && g->def->a) *t = evaltype(g->def->a, g->env);
  return &g->v;
}

static Val *evallist(Node *n, Frame *f, int *len) {
  int cap = n->n, k = 0;
  Val *v = alloc(cap * sizeof(Val));
  for (int i = 0; i < n->n; i++) {
    Node *e = n->v[i];
    Val x = eval(e->k == N_SPREAD ? e->a : e, f);
    int m = e->k == N_SPREAD ? (x.k == K_ARR ? x.a->len : (trap(e, "spread of a non-array"), 0)) : 1;
    if (k + m > cap) {
      Val *w = alloc((cap = 2 * (k + m)) * sizeof(Val));
      v = memcpy(w, v, k * sizeof(Val));
    }
    if (e->k == N_SPREAD) memcpy(v + k, x.a->v, m * sizeof(Val));
    else v[k] = x;
    k += m;
  }
  *len = k;
  return v;
}

static Obj *mkobject(Node *n, Frame *f) {
  if (!n->names) {
    n->names = alloc(n->n * sizeof(Sym *));
    for (int i = 0; i < n->n; i++) n->names[i] = n->v[i]->sym;
  }
  Obj *o = mkobj(n->n, n->names);
  o->lit = 1;
  if (!(n->flags & F_SPREAD)) {
    for (int i = 0; i < n->n; i++) o->v[i] = n->v[i]->a ? eval(n->v[i]->a, f) : FN(bound(closure(n->v[i]->c, f), O(o)));
    return o;
  }
  o->n = 0;
  Class *c = 0;
  for (int i = 0; i < n->n; i++) {
    Node *p = n->v[i];
    Val x = p->a ? eval(p->a, f) : FN(bound(closure(p->c, f), O(o)));
    Obj *src = p->flags & F_SPREAD ? (x.k == K_OBJ ? x.o : (trap(p, "spread of a non-object"), NULL)) : 0;
    if (src && src->cls) c = src->cls;
    for (int j = 0; j < (src ? src->n : 1); j++) {
      Sym *s = src ? src->names[j] : p->sym;
      int k = field(o, s, 0);
      if (k < 0) {
        if (o->n % 8 == 0) {
          Sym **names = memcpy(alloc((o->n + 8) * sizeof(Sym *)), o->names, o->n * sizeof(Sym *));
          o->v = memcpy(alloc((o->n + 8) * sizeof(Val)), o->v, o->n * sizeof(Val));
          o->names = names;
        }
        o->names[k = o->n++] = s;
      }
      o->v[k] = src ? src->v[j] : x;
    }
  }
  if (!c) return o;
  // A class spread makes an instance: its fields in the class's order, from o by name.
  Obj *r = mkobj(c->nf, c->fnames);
  r->cls = c, r->lit = 1;
  for (int i = 0; i < c->nf; i++) r->v[i] = o->v[field(o, c->fnames[i], 0)];
  return r;
}

static void bind(Node *p, Val v, Frame *f) {
  if (p->k == P_ID) {
    f->s[p->slot] = v;
    return;
  }
  if (p->k == P_ARR && v.k != K_ARR) trap(p, "not an array");
  if (p->k == P_OBJ && v.k != K_OBJ) trap(p, "not an object");
  for (int i = 0; i < p->n; i++) {
    Node *e = p->v[i];
    Val x;
    if (e->flags & F_REST && p->k == P_ARR) {
      Arr *a = mkarr(v.a->len > i ? v.a->len - i : 0);
      memcpy(a->v, v.a->v + i, a->len * sizeof(Val));
      a->elem = v.a->elem;
      x = A(a);
    } else if (e->flags & F_REST) {
      Obj *r = mkobj(0, alloc(v.o->n * sizeof(Sym *)));
      r->v = alloc(v.o->n * sizeof(Val));
      for (int j = 0; j < v.o->n; j++) {
        int named = 0;
        for (int q = 0; q < p->n; q++) named |= p->v[q]->sym == v.o->names[j];
        if (!named) r->names[r->n] = v.o->names[j], r->v[r->n++] = v.o->v[j];
      }
      x = O(r);
    } else if (p->k == P_ARR) {
      if (i >= v.a->len) trap(e, "array pattern longer than array");
      x = v.a->v[i];
    } else x = member(v, e, 1);
    if (x.k == K_NULL && e->b) x = deflt(eval(e->b, f));
    bind(e->c, x, f);
  }
}

static Val instantiate(Class *c, Val *args, int na, Node *site) {
  Obj *o = mkobj(c->nf, c->fnames);
  o->cls = c;
  Node *d = c->def;
  Frame *f = FRAME(d);
  f->up = c->frame->up, f->this = O(o);
  for (int i = 0; i < c->nf; i++) o->v[i] = decl(c->fprops[i], f);
  if (c->ctor) call(c->ctor, O(o), args, na, site);
  return O(o);
}

static Val construct(Type *t, Val *args, int na, Node *n) {
  if (t->tk == T_CLASS) return instantiate(t->cls, args, na, n);
  if (t->tk == T_TUP) return zero(t);
  if (t->tk != T_ARR || t->n >= 0) trap(n, "cannot construct");
  if (!na || !isint(args[0].k) || args[0].i < 0) trap(n, "negative length");
  Arr *a = mkarr(args[0].i);
  a->elem = t->elem;
  for (int i = 0; i < a->len; i++) a->v[i] = zero(t->elem);
  return A(a);
}

static Obj *fresh(Class *c, Val v, Node *site) {
  if (v.k != K_OBJ || !v.o->lit) trap(site, "copy into an embedded field with {...v}");
  Obj *o = instantiate(c, 0, 0, site).o;
  for (int i = 0; i < v.o->n; i++) {
    int k = field(o, v.o->names[i], 0);
    if (k < 0) trap(site, "no such field");
    Node *d = c->fprops[k];
    o->v[k] = store(d->a ? evaltype(d->a, c->frame) : 0, o->v[k], v.o->v[i]);
  }
  return o;
}

static Val mkclass(Node *d, Frame *env) {
  Class *c = alloc(sizeof(Class));
  c->def = d;
  c->frame = alloc(fsize(d));
  c->frame->up = env;
  c->fnames = alloc(d->n * sizeof(Sym *)), c->fprops = alloc(d->n * sizeof(Node *));
  for (int i = 0; i < d->n; i++) {
    Node *p = d->v[i];
    Fn *fn = p->c ? closure(p->c, c->frame) : 0;
    if (p->flags & F_STATIC) global(&c->statics, p->sym, fn ? 0 : p, fn ? FN(fn) : NUL)->env = c->frame;
    else if (fn && p->sym == s_constructor) c->ctor = fn;
    else if (fn) global(&c->methods, p->sym, 0, FN(fn));
    else c->fnames[c->nf] = p->sym, c->fprops[c->nf++] = p;
  }
  for (int i = 0; i < d->ntp; i++) c->frame->s[i] = TY(anytype);
  Type *t = newtype(T_CLASS, 0);
  t->cls = c;
  c->type = t;
  return TY(t);
}

/// Generic class c with the type args of n, the rest by their defaults: a class of its own, made once per args.
static Class *instance(Class *c, Node *n, Frame *f) {
  c = origin(c);
  Node *d = c->def;
  Type *args[d->ntp];
  for (int i = 0; i < d->ntp; i++) args[i] = i < n->n ? evaltype(n->v[i], f) : 0;
  for (Class *k = c->instances; k; k = k->next) {
    int same = 1;
    for (int i = 0; i < n->n && i < d->ntp; i++) same &= k->frame->s[i].t == args[i];
    if (same) return k;
  }
  Class *k = mkclass(d, c->frame->up).t->cls;
  k->generic = c, k->next = c->instances, c->instances = k;
  for (int i = 0; i < d->ntp; i++)
    k->frame->s[i] = TY(args[i] ? args[i] : d->tdefs[i] ? evaltype(d->tdefs[i], k->frame) : anytype);
  return k;
}

_Static_assert(sizeof(int) == 4 && sizeof(long) == 8 && sizeof(void *) == 8 && sizeof(size_t) == 8 &&
  __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "std/c fixes LP64, and calls into C need little-endian");

static ffi_type *ctypes[] = {&ffi_type_void, [K_BOOL] = &ffi_type_uint8, [K_I8] = &ffi_type_sint8,
  &ffi_type_sint16, &ffi_type_sint32, &ffi_type_sint64, &ffi_type_uint8, &ffi_type_uint16, &ffi_type_uint32,
  &ffi_type_uint64, [K_F32] = &ffi_type_float, &ffi_type_double};


static Type *fieldtype_c(Type *t, int i) {
  if (t->tk == T_ARR) return t->elem;
  Node *p = t->cls->fprops[i];
  return p->a ? evaltype(p->a, t->cls->frame) : 0;
}

static ffi_type *cty(Type *t, int held, Node *site) {
  if (t && t->tk == T_PRIM && t->prim && t->prim <= K_F64 && ctypes[t->prim]) return ctypes[t->prim];
  int fixed = t && t->tk == T_ARR && t->n >= 0;
  if (!t || (!fixed && (!held || t->tk != T_CLASS))) trap(site, "not a C type");
  int n = nfields(t);
  ffi_type *s = alloc(sizeof *s), **el = alloc((n + 1) * sizeof *el);
  for (int i = 0; i < n; i++)
    el[i] = cty(fieldtype_c(t, i), t->tk == T_ARR || t->cls->fprops[i]->flags & F_EMBED, site);
  *s = (ffi_type){.type = FFI_TYPE_STRUCT, .elements = el};
  if (ffi_get_struct_offsets(FFI_DEFAULT_ABI, s, 0) != FFI_OK) trap(site, "not a C type");
  return s;
}

static ffi_type *ctype(Node *type, Frame *f, Node *site) {
  return type ? cty(evaltype(type, f), 1, site) : &ffi_type_void;
}

static void put(Type *t, ffi_type *ct, Val v, char *p, Node *site) {
  if (ct->type != FFI_TYPE_STRUCT) {
    int k = t->prim;
    Val a = conv(prims[k], v);
    if (k == K_BOOL ? a.k != K_BOOL : !isnum(a.k)) trap(site, "not a C value");
    union { int64_t i; float f; double d; } u;
    if (k == K_F32) u.f = a.f;
    else if (k == K_F64) u.d = a.f;
    else u.i = a.i;
    memcpy(p, &u, ct->size);
    return;
  }
  if (!is(v, t) || (v.k == K_ARR && v.a->len != t->n)) trap(site, "not a C value");
  size_t off[nfields(t) + 1];
  ffi_get_struct_offsets(FFI_DEFAULT_ABI, ct, off);
  Val *s = v.k == K_ARR ? v.a->v : v.o->v;
  for (int i = 0; i < nfields(t); i++) put(fieldtype_c(t, i), ct->elements[i], s[i], p + off[i], site);
}

static Val get(Type *t, ffi_type *ct, char *p) {
  if (ct->type == FFI_TYPE_VOID) return NUL;
  if (ct->type != FFI_TYPE_STRUCT) {
    int k = t->prim;
    union { int64_t i; float f; double d; } u = {0};
    memcpy(&u, p, ct->size);
    return k == K_BOOL ? B(u.i) : k == K_F32 ? F(k, u.f) : k == K_F64 ? F(k, u.d) : cast(k, I(K_U64, u.i));
  }
  int n = nfields(t);
  size_t off[n + 1];
  ffi_get_struct_offsets(FFI_DEFAULT_ABI, ct, off);
  Val v = t->tk == T_CLASS ? O(mkobj(n, t->cls->fnames)) : A(mkarr(n));
  if (t->tk == T_CLASS) v.o->cls = t->cls;
  if (t->tk == T_ARR) v.a->elem = t->elem;
  Val *s = v.k == K_ARR ? v.a->v : v.o->v;
  for (int i = 0; i < n; i++) s[i] = get(fieldtype_c(t, i), ct->elements[i], p + off[i]);
  return v;
}

/// Whether param q of a C function is a struct through a pointer: a class, not ...T (3.1).
static int byref(Node *q, Frame *f) { return !(q->flags & F_EMBED) && q->a && evaltype(q->a, f)->tk == T_CLASS; }

/// The array member of t if param q of a C function is a pointer that takes string | T[] | u64 (3.1), else 0.
static Type *elems(Node *q, Frame *f) {
  Type *t = q->a ? evaltype(q->a, f) : 0;
  for (int i = 0; t && t->tk == T_UNION && i < t->n; i++)
    if (t->mem[i]->tk == T_ARR) return t->mem[i];
  return 0;
}

static Fn *external(Node *d, Frame *f) {
  Fn *fn = closure(d, f);
  fn->sym = dlsym(RTLD_DEFAULT, d->sym->s);  // if missing, a call traps: bober imports TPDE, which it calls only to debug
  ffi_type **ts = alloc((d->n + 1) * sizeof *ts);
  for (int i = 0; i < d->n; i++)
    ts[i] = byref(d->v[i], f) || elems(d->v[i], f) ? &ffi_type_pointer : ctype(d->v[i]->a, f, d->v[i]);
  fn->cif = alloc(sizeof(ffi_cif));
  if (ffi_prep_cif(fn->cif, FFI_DEFAULT_ABI, d->n, ctype(d->b, f, d), ts) != FFI_OK) trap(d, "not a C signature");
  return fn;
}

static Val ccall(Fn *fn, Val *args, int na, Node *site) {
  Node *d = fn->def;
  if (!fn->sym) trap(site, "%s is not in the process", d->sym->s);
  if (na != d->n) trap(site, "%s takes %d arguments", d->sym->s, d->n);
  ffi_cif *cif = fn->cif;
  void *p[na + 1];
  char *refs[na + 1];  // the copy of each instance passed through a pointer, which C's changes go back from
  ffi_type *ct[na + 1];
  char *bufs[na + 1];  // the copy of each string or array passed through a pointer, which an array's changes go back from
  for (int i = 0; i < na; i++) {
    Type *t = evaltype(d->v[i]->a, fn->env), *e = elems(d->v[i], fn->env);
    refs[i] = byref(d->v[i], fn->env) ? alloca((ct[i] = cty(t, 1, site))->size) : 0;
    bufs[i] = 0;
    if (refs[i]) put(t, ct[i], args[i], refs[i], site), p[i] = &refs[i];
    else if (e && args[i].k == K_STR) {
      bufs[i] = calloc(args[i].s->len + 1, 1), p[i] = &bufs[i];
      memcpy(bufs[i], args[i].s->p, args[i].s->len);
    } else if (e && args[i].k == K_ARR) {
      int j = 0;
      while (j < i && !(bufs[j] && args[j].k == K_ARR && args[j].a == args[i].a)) j++;
      if (j < i) {  // an array passed twice is one copy
        p[i] = &bufs[j];
        continue;
      }
      ct[i] = cty(e->elem, 1, site), bufs[i] = calloc(args[i].a->len + 1, ct[i]->size), p[i] = &bufs[i];
      for (int j = 0; j < args[i].a->len; j++) put(e->elem, ct[i], args[i].a->v[j], bufs[i] + j * ct[i]->size, site);
    } else put(e ? prims[K_U64] : t, cif->arg_types[i], args[i], p[i] = alloca(cif->arg_types[i]->size), site);
  }
  char *r = alloca(cif->rtype->size > sizeof(ffi_arg) ? cif->rtype->size : sizeof(ffi_arg));
  ffi_call(cif, fn->sym, r, p);
  for (int i = 0; i < na; i++) {  // in fresh values, so an embedded instance is replaced, not changed
    Type *t = evaltype(d->v[i]->a, fn->env), *e = elems(d->v[i], fn->env);
    if (refs[i]) memcpy(args[i].o->v, get(t, ct[i], refs[i]).o->v, nfields(t) * sizeof(Val));
    if (bufs[i] && args[i].k == K_ARR)
      for (int j = 0; j < args[i].a->len; j++) args[i].a->v[j] = get(e->elem, ct[i], bufs[i] + j * ct[i]->size);
    free(bufs[i]);
  }
  return get(d->b ? evaltype(d->b, fn->env) : 0, cif->rtype, r);
}

static int subtest(Node *n, Frame *f) {
  Str *outer = testpath;
  testpath = cat(cat(outer, mkstr(" / ", 3)), tostr(eval(n->a, f)));
  jmp_buf up;
  memcpy(up, trapjb, sizeof up);
  Handler *h = handlers;
  int d = depth;
  handlers = 0;
  if (setjmp(trapjb)) printf("FAIL %.*s: %s\n", testpath->len, testpath->p, trapmsg), subfails++;
  else if (exec(n->b, f) >= S_BRK) trap(n, "break or continue out of a test");
  memcpy(trapjb, up, sizeof up);
  handlers = h, depth = d, testpath = outer;
  return S_NORMAL;
}

static Val cimport(Node *imp);

static Val decl(Node *d, Frame *f) {
  Type *t;
  switch (d->k) {
  case N_FN: return FN(d->flags & F_EXTERN ? external(d, f) : closure(d, f));
  case N_IMPORT: return cimport(d);
  case N_CLASS: return mkclass(d, f);
  case N_TYPEDEF: return TY(evaltype(d->a, f));
  case N_IFACE:
    t = newtype(T_OBJ, d->n);
    t->names = alloc(d->n * sizeof(Sym *));
    for (int i = 0; i < d->n; i++) t->names[i] = d->v[i]->sym, t->mem[i] = d->v[i]->a ? evaltype(d->v[i]->a, f) : 0;
    return TY(t);
  }
  t = d->a ? evaltype(d->a, f) : 0;
  if (d->flags & F_EXTERN) {
    void *p = dlsym(RTLD_DEFAULT, d->c->sym->s);
    if (!p) trap(d, "%s", dlerror());
    return get(t, cty(t, 1, d), p);
  }
  if (d->flags & F_EMBED) return d->b ? O(fresh(t->cls, eval(d->b, f), d)) : construct(t, 0, 0, d);
  Val v = d->b ? eval(d->b, f) : zero(t);
  return !t && d->op && (v.k == K_INT || v.k == K_FLT) ? v : conv(t, v);
}

static Val call(Fn *fn, Val self, Val *args, int na, Node *site) {
  if (fn->self.k) self = fn->self;
  if (fn->bi) return fn->bi(fn->nt ? fn->targs[0] : self, args, na, site);  // load[T] gets T as its self
  if (fn->sym) return ccall(fn, args, na, site);
  Node *d = fn->def;
  if (++depth > 100000) trap(site, "stack overflow");
  Frame *f = FRAME(d);
  f->up = fn->env, f->this = d->flags & F_METHOD ? self : fn->env ? fn->env->this : NUL;
  for (int i = 0; i < d->ntp; i++) f->s[i] = i < fn->nt ? fn->targs[i] : TY(anytype);
  for (int i = 0; i < d->n; i++) {
    Node *p = d->v[i];
    Val v = i < na ? args[i] : p->b ? eval(p->b, f) : NUL;
    if (p->flags & F_REST) {
      Arr *a = mkarr(na > i ? na - i : 0);
      memcpy(a->v, args + i, a->len * sizeof(Val));
      a->lit = 1, v = A(a);
    }
    // A type param not given that is a param's type is its argument's, by value, as the checker would infer it.
    Node *t = p->a;
    if (t && t->k == N_LOCAL && !t->depth && t->slot >= fn->nt && t->slot < d->ntp && f->s[t->slot].t == anytype)
      f->s[t->slot] = TY(typeofv(v));
    bind(p->c, conv(p->a ? evaltype(p->a, f) : 0, v), f);
  }
  Val r = !d->c ? NUL : d->c->k != N_BLOCK ? eval(d->c, f) : exec(d->c, f) == S_RET ? retval : NUL;
  depth--;
  return conv(d->b ? evaltype(d->b, f) : 0, r);
}

static Val evalcall(Node *n, Frame *f) {
  Val self = NUL, c;
  if (n->a->k == N_MEMBER) {
    self = eval(n->a->a, f);
    if (self.k == K_NULL && n->a->flags & F_OPT) return NUL;
    c = member(self, n->a, 0);
  } else c = eval(n->a, f);
  if (c.k == K_NULL && n->flags & F_OPT) return NUL;
  if (c.k != K_FN) trap(n, "not a function");
  int na = n->n;
  Val *args;
  if (n->flags & F_SPREAD) args = evallist(n, f, &na);
  else {
    args = alloca(na * sizeof(Val));
    for (int i = 0; i < na; i++) args[i] = eval(n->v[i], f);
  }
  return call(c.fn, self, args, na, n);
}

static int chain(Node *n, Frame *f, Val *rhs) {
  Val l;
  if (n->flags & F_CHAIN) {
    if (!chain(n->a, f, &l)) return 0;
  } else l = eval(n->a, f);
  *rhs = eval(n->b, f);
  return relop(n->op, l, *rhs, n);
}

static Node *pickcase(Node *n, Frame *f) {
  Val s = eval(n->a, f);
  Node *dflt = 0;
  for (int i = 0; i < n->n; i++) {
    Node *c = n->v[i];
    if (!c->n) dflt = c;
    for (int j = 0; j < c->n; j++) {
      Node *l = c->v[j];
      if (l->k == N_RANGE) {
        if (relop(GE, s, eval(l->a, f), l) && relop(LE, s, eval(l->b, f), l)) return c;
        continue;
      }
      Val v = eval(l, f);
      if (v.k == K_TYPE && s.k != K_TYPE ? is(s, v.t) : eq(s, v)) return c;
    }
  }
  return dflt;
}

static Val eval(Node *n, Frame *f) {
  Val a, b;
  Node *c;
  int len;
  switch (n->k) {
  case N_LIT: return n->val;
  case N_LOCAL: return *local(n, f);
  case N_GLOBAL: return gval(gref(n));
  case N_THIS: return f ? f->this : NUL;
  case N_TPL: {
    Str *s = n->v[0]->val.s;
    for (int i = 1; i < n->n; i++) {
      Val v = eval(n->v[i], f);
      Fn *m = v.k == K_OBJ ? method(v.o->cls, s_toString, 0) : 0;
      s = cat(s, m ? call(m, v, 0, 0, n).s : tostr(v));
    }
    return S(s);
  }
  case N_ARRAY: {
    Val *v = evallist(n, f, &len);
    Arr *arr = alloc(sizeof(Arr));
    arr->len = arr->cap = len, arr->v = v, arr->lit = 1;
    return A(arr);
  }
  case N_OBJECT: return O(mkobject(n, f));
  case N_FN: return FN(closure(n, f));
  case N_UNARY:
    a = eval(n->a, f);
    if (n->op == '!') return B(!truth(a));
    if (n->op == TYPEOF) return TY(typeofv(a));
    if (!isnum(a.k)) trap(n, "bad operand");
    if (isflt(a.k)) return F(a.k, -a.f);
    return I(a.k, wrap(a.k, n->op == '-' ? -(uint64_t)a.i : ~a.i));
  case N_BIN:
    a = eval(n->a, f), b = eval(n->b, f);
    if (n->op == '|' && !(isint(a.k) && isint(b.k))) return TY(unite(totype(a), totype(b)));
    return arith(n->op, a, b, n);
  case N_CMP: return B(chain(n, f, &a));
  case N_AND: return B(truth(eval(n->a, f)) && truth(eval(n->b, f)));
  case N_OR: return B(truth(eval(n->a, f)) || truth(eval(n->b, f)));
  case N_QQ: a = eval(n->a, f); return a.k != K_NULL ? a : eval(n->b, f);
  case N_IS: a = eval(n->a, f); return B(is(a, evaltype(n->b, f)));
  case N_AS: {
    Type *t = evaltype(n->b, f);
    a = eval(n->a, f);
    if (t->tk == T_PRIM && t->prim == K_BOOL && isnum(a.k)) return B(isflt(a.k) ? a.f != 0 : a.i != 0);
    if (t->tk == T_PRIM && isnum(t->prim) && (isnum(a.k) || a.k == K_BOOL)) return cast(t->prim, a);
    if (t->tk == T_PRIM && isint(t->prim) && a.k == K_STR) return codepoint(t->prim, a.s, n);
    if (!is(a, t)) trap(n, "cast to a type that is not the value's");
    return conv(t, a);
  }
  case N_IFX: return eval(truth(eval(n->a, f)) ? n->b : n->c, f);
  case N_SWITCHX:
    if (!(c = pickcase(n, f))) trap(n, "no case matched");
    return eval(c->a, f);
  case N_NONNULL:
    if ((a = eval(n->a, f)).k == K_NULL) trap(n, "null");
    return a;
  case N_MEMBER:
    a = eval(n->a, f);
    return a.k == K_NULL && n->flags & F_OPT ? NUL : member(a, n, 1);
  case N_INDEX:
    a = eval(n->a, f);
    return a.k == K_NULL && n->flags & F_OPT ? NUL : subscript(a, n, f);
  case N_CALL: return evalcall(n, f);
  case N_NEW: {
    Type *t = evaltype(n->a, f);
    Val *v = evallist(n, f, &len);
    return construct(t, v, len, n);
  }
  }
  trap(n, "not an expression");
}

static int protect(Node *body, Frame *f, int *st, Val *exc) {
  Handler h = {handlers, .depth = depth};
  handlers = &h;
  if (setjmp(h.jb)) {
    handlers = h.up, depth = h.depth, *exc = thrown;
    return 1;
  }
  *st = exec(body, f);
  handlers = h.up;
  return 0;
}

static int try(Node *n, Frame *f) {
  int st = S_NORMAL, pending = 0;
  Val exc;
  if (protect(n->a, f, &st, &exc)) {
    Node *c = n->b;
    pending = 1;
    if (c && (!c->a || is(exc, evaltype(c->a, f)))) {
      if (c->c) bind(c->c, exc, f);
      pending = protect(c->b, f, &st, &exc);
    }
  }
  if (n->c) {
    Val r = retval;
    int fst = exec(n->c, f);
    if (fst) return fst;
    retval = r;
  }
  if (pending) throwv(exc);
  return st;
}

static void assign(Val *p, Type *t, int embed, Val v, Node *site) {
  if (embed) memcpy(p->o->v, fresh(p->o->cls, v, site)->v, p->o->n * sizeof(Val));
  else *p = store(t, *p, v);
}

static Frame *enter(Frame *it, Frame *f) {
  it->up = f, it->this = f->this;
  return it;
}

/// The frame of the iteration of loop n after it, or of its first: a new one if a closure may keep it (5.1).
#define NEXT(n, f, it) ((it) && !((n)->flags & F_HEAP) ? (it) : enter(FRAME(n), f))

static int exec(Node *n, Frame *f) {
  Val a, b, *p;
  Type *t;
  Node *c;
  Frame *it = 0;
  Arr *in;
  int st, embed;
  switch (n->k) {
  case N_BLOCK:
    for (int i = 0; i < n->n; i++) if ((st = exec(n->v[i], f))) return st;
    return S_NORMAL;
  case N_VAR: bind(n->c, decl(n, f), f); return S_NORMAL;
  case N_FN:
  case N_CLASS:
  case N_IFACE:
  case N_TYPEDEF: define(&f->s[n->slot], n, f); return S_NORMAL;
  case N_IF:
    if (truth(eval(n->a, f))) return exec(n->b, f);
    return n->c ? exec(n->c, f) : S_NORMAL;
  case N_WHILE:
    while (truth(eval(n->a, it = NEXT(n, f, it)))) {
      if ((st = exec(n->b, it)) == S_RET) return st;
      if (st == S_BRK) break;
    }
    return S_NORMAL;
  case N_FOR:
    a = eval(n->a, it = NEXT(n, f, it));
    if (n->b) {
      b = eval(n->b, it);
      int k = join(a.k, b.k);
      if (k == K_INT) k = K_I32;
      if (!isint(k)) trap(n, "range bounds are not integers");
      a = cast(k, a), b = cast(k, b);
      for (int64_t i = a.i; isuns(k) ? (uint64_t)i <= (uint64_t)b.i : i <= b.i; i++, it = NEXT(n, f, it)) {
        bind(n->c, I(k, i), it);
        if ((st = exec(n->d, it)) == S_RET) return st;
        if (st == S_BRK) break;
      }
      return S_NORMAL;
    }
    if (a.k != K_ARR && a.k != K_STR) trap(n, "not an array or string");
    for (int i = 0; i < (a.k == K_STR ? a.s->len : a.a->len); i++, it = NEXT(n, f, it)) {
      bind(n->c, a.k == K_STR ? I(K_U8, (uint8_t)a.s->p[i]) : a.a->v[i], it);
      if ((st = exec(n->d, it)) == S_RET) return st;
      if (st == S_BRK) break;
    }
    return S_NORMAL;
  case N_SWITCH: return (c = pickcase(n, f)) ? exec(c->a, f) : S_NORMAL;
  case N_TRY: return try(n, f);
  case N_RETURN: retval = n->a ? eval(n->a, f) : NUL; return S_RET;
  case N_BREAK: return S_BRK;
  case N_CONTINUE: return S_CONT;
  case N_THROW:
    a = deflt(eval(n->a, f));
    thrownat = n;
    throwv(a);
  case N_TEST: return subtest(n, f);
  case N_ASSERT:
    if (n->a->k == N_CMP && n->a->op == EQ && !(n->a->flags & F_CHAIN)) {
      Val l = eval(n->a->a, f), r = eval(n->a->b, f);
      if (eq(l, r)) return S_NORMAL;
      Str *x = tostr(l), *y = tostr(r);
      trap(n, "assert failed: %.*s != %.*s", x->len, x->p, y->len, y->p);
    }
    if (!truth(eval(n->a, f))) trap(n, "assert failed");
    return S_NORMAL;
  case N_ASSIGN:
    if (n->a->k == N_ARRAY) {
      b = eval(n->b, f);
      if (b.k != K_ARR || b.a->len < n->a->n) trap(n, "array pattern longer than array");
      for (int i = 0; i < n->a->n; i++) p = lref(n->a->v[i], f, &t, &embed, &in), assign(p, t, embed, b.a->v[i], n);
      return S_NORMAL;
    }
    // The target, then its value, then the right side (7.1), which may move the elements the target is in.
    p = lref(n->a, f, &t, &embed, &in), a = *p;
    if (n->op == QQ && a.k != K_NULL) return S_NORMAL;
    long at = in ? p - in->v : 0;
    b = eval(n->b, f);
    if (in) p = &in->v[idx(I(K_I64, at), in->len, n)];
    assign(p, t, embed, n->op == '=' || n->op == QQ ? b : arith(n->op, a, b, n), n);
    return S_NORMAL;
  }
  eval(n, f);
  return S_NORMAL;
}

static Val bi_print(Val self, Val *a, int n, Node *site) {
  Str *s = tostr(n ? a[0] : NUL);
  fwrite(s->p, 1, s->len, stdout);
  putchar('\n');
  return NUL;
}

static char *cstr(Val v, Node *site) {
  if (v.k != K_STR) trap(site, "not a string");
  char *p = alloc(v.s->len + 1);
  memcpy(p, v.s->p, v.s->len);
  return p;
}

static Str *slurp(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return 0;
  Str *s = mkstr("", 0);
  long cap = 4096, n;
  s->p = alloc(cap);
  while ((n = fread(s->p + s->len, 1, cap - s->len, fp)) > 0)
    if ((s->len += n) == cap) s->p = memcpy(alloc(cap *= 2), s->p, s->len);
  fclose(fp);
  return s;
}

static Val bi_readFile(Val self, Val *a, int n, Node *site) {
  Str *s = slurp(cstr(n ? a[0] : NUL, site));
  return s ? S(s) : NUL;
}

static Val bi_writeFile(Val self, Val *a, int n, Node *site) {
  FILE *fp = fopen(cstr(n ? a[0] : NUL, site), "wb");
  if (!fp) return B(0);
  int ok = n > 1 && a[1].k == K_STR && fwrite(a[1].s->p, 1, a[1].s->len, fp) == (size_t)a[1].s->len;
  return B(!fclose(fp) && ok);
}

static Val bi_args(Val self, Val *a, int n, Node *site) {
  int from = argc_ > 3 && !strcmp(argv_[3], "--") ? 4 : 3;  // as native bober, a -- after the file is dropped
  Arr *r = mkarr(argc_ - from);
  for (int i = 0; i < r->len; i++) r->v[i] = S(mkstr(argv_[i + from], strlen(argv_[i + from])));
  return A(r);
}

/// The operating system and CPU this is built for, as std/os names them.
#if defined(__APPLE__)
#define TARGET_OS "macos"
#elif defined(__FreeBSD__)
#define TARGET_OS "freebsd"
#elif defined(_WIN32)
#define TARGET_OS "windows"
#else
#define TARGET_OS "linux"
#endif
#if defined(__aarch64__)
#define TARGET_ARCH "arm64"
#else
#define TARGET_ARCH "x86_64"
#endif

static Val bi_targetOs(Val self, Val *a, int n, Node *site) { return S(mkstr(TARGET_OS, strlen(TARGET_OS))); }

static Val bi_targetArch(Val self, Val *a, int n, Node *site) { return S(mkstr(TARGET_ARCH, strlen(TARGET_ARCH))); }

static Val bi_now(Val self, Val *a, int n, Node *site) {
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return I(K_I64, t.tv_sec * 1000000000 + t.tv_nsec);
}

/// A reference's address, as nothing moves or is freed.
static Val bi_idOf(Val self, Val *a, int n, Node *site) { return I(K_U64, n ? a[0].i : 0); }

static Val bi_chr(Val self, Val *a, int n, Node *site) {
  char c = n ? a[0].i : 0;
  return S(mkstr(&c, 1));
}

static Val bi_fromBytes(Val self, Val *a, int n, Node *site) {
  if (!n || a[0].k != K_ARR) trap(site, "not an array");
  Str *s = mkstr("", 0);
  s->p = alloc(s->len = a[0].a->len);
  for (int i = 0; i < s->len; i++) s->p[i] = a[0].a->v[i].i;
  return S(s);
}

/// std/c's C memory, which rt.c gives and boot does not link; bi_toC's strndup must match it.
void *bober_malloc(uint64_t n) { return malloc(n ? n : 1); }
void bober_free(void *p) { free(p); }

static Val bi_toC(Val self, Val *a, int n, Node *site) {
  if (!n || a[0].k != K_STR || memchr(a[0].s->p, 0, a[0].s->len)) trap(site, "not a C string");
  return I(K_U64, (intptr_t)strndup(a[0].s->p, a[0].s->len));
}

static Val bi_fromC(Val self, Val *a, int n, Node *site) {
  char *p = n ? (char *)(intptr_t)a[0].i : 0;
  if (!p) trap(site, "null C string");
  return S(mkstr(p, strlen(p)));
}

static Val bi_fromCN(Val self, Val *a, int n, Node *site) {
  char *p = (char *)(intptr_t)a[0].i;
  if (!p && a[1].i) trap(site, "null C string");
  if ((uint64_t)a[1].i > INT32_MAX) trap(site, "string too long");
  return S(mkstr(p, a[1].i));
}

static Val bi_toCN(Val self, Val *a, int n, Node *site) {
  if (!n || a[0].k != K_STR) trap(site, "not a string");
  char *p = malloc(a[0].s->len + 1);
  p[a[0].s->len] = 0;
  return I(K_U64, (intptr_t)memcpy(p, a[0].s->p, a[0].s->len));
}

static Val bi_peek(Val self, Val *a, int n, Node *site) {
  if (!n || !a[0].i) trap(site, "load from null");
  return I(K_U64, *(uint64_t *)(intptr_t)a[0].i);
}

static Val bi_poke(Val self, Val *a, int n, Node *site) {
  if (n < 2 || !a[0].i) trap(site, "store to null");
  *(uint64_t *)(intptr_t)a[0].i = a[1].i;
  return NUL;
}

/// builtin/mem's addr: the address of a copy of x, as C holds it.
static Val bi_addr(Val self, Val *a, int n, Node *site) {
  if (!n) trap(site, "addr needs a value");
  Type *t = typeofv(a[0]);
  ffi_type *ct = cty(t, 1, site);
  char *p = alloc(ct->size);
  put(t, ct, a[0], p, site);
  return I(K_U64, (intptr_t)p);
}

/// builtin/mem's load[T]: the T at address a[0], as C holds it.
static Val bi_load(Val self, Val *a, int n, Node *site) {
  if (self.k != K_TYPE || !n || !a[0].i) trap(site, "load from null");
  return get(self.t, cty(self.t, 1, site), (char *)(intptr_t)a[0].i);
}

static Val bi_slice(Val self, Val *a, int n, Node *site) {
  int len = self.k == K_STR ? self.s->len : self.a->len;
  if (n < 2 || !isint(a[0].k) || !isint(a[1].k) || a[0].i < 0 || a[0].i > a[1].i || a[1].i > len)
    trap(site, "slice out of range");
  int lo = a[0].i, m = a[1].i - lo;
  if (self.k == K_STR) {
    Str *s = alloc(sizeof(Str));
    s->len = m, s->p = self.s->p + lo;
    return S(s);
  }
  Arr *r = mkarr(m);
  memcpy(r->v, self.a->v + lo, m * sizeof(Val));
  r->elem = self.a->elem;
  return A(r);
}

static Val bi_at(Val self, Val *a, int n, Node *site) {
  int len = self.k == K_STR ? self.s->len : self.a->len;
  if (n != 1 || !isint(a[0].k)) trap(site, "at takes an index");
  if (a[0].i < 0 || a[0].i >= len) return NUL;
  return self.k == K_STR ? I(K_U8, (uint8_t)self.s->p[a[0].i]) : self.a->v[a[0].i];
}

static Val bi_copy(Val self, Val *a, int n, Node *site) { return self; }  // what it keeps alive does not matter here

static Val bi_push(Val self, Val *a, int n, Node *site) {
  Arr *r = self.a;
  if (n != 1 && !(site->flags & F_SPREAD)) trap(site, "push takes one element");
  for (int i = 0; i < n; i++) {
    if (r->len == r->cap) r->v = memcpy(alloc((r->cap = 2 * r->cap + 8) * sizeof(Val)), r->v, r->len * sizeof(Val));
    r->v[r->len++] = conv(r->elem, a[i]);
  }
  return NUL;
}

static Val bi_append(Val self, Val *a, int n, Node *site) {
  if (n != 1 || a[0].k != K_ARR) trap(site, "append takes an array");
  Arr *xs = a[0].a;
  return bi_push(self, xs->v, xs->len, &(Node){.flags = F_SPREAD});
}

static Val bi_map(Val self, Val *a, int n, Node *site) {
  if (n != 1 || a[0].k != K_FN) trap(site, "map takes a function");
  Arr *r = mkarr(self.a->len);
  r->lit = 1;
  for (int i = 0; i < r->len; i++) r->v[i] = call(a[0].fn, NUL, &self.a->v[i], 1, site);
  return A(r);
}

static Val bi_filter(Val self, Val *a, int n, Node *site) {
  if (n != 1 || a[0].k != K_FN) trap(site, "filter takes a function");
  Arr *r = mkarr(self.a->len);
  r->elem = self.a->elem, r->len = 0;
  for (int i = 0; i < self.a->len; i++)
    if (call(a[0].fn, NUL, &self.a->v[i], 1, site).i) r->v[r->len++] = self.a->v[i];
  return A(r);
}

/* Whether f holds for some element if any, else for every one. */
static Val test(Val self, Val *a, int n, Node *site, int any) {
  if (n != 1 || a[0].k != K_FN) trap(site, "some and every take a function");
  for (int i = 0; i < self.a->len; i++)
    if (!call(a[0].fn, NUL, &self.a->v[i], 1, site).i == !any) return B(any);
  return B(!any);
}

static Val bi_some(Val self, Val *a, int n, Node *site) { return test(self, a, n, site, 1); }
static Val bi_every(Val self, Val *a, int n, Node *site) { return test(self, a, n, site, 0); }

static Val bi_find(Val self, Val *a, int n, Node *site) {
  if (n != 1 || a[0].k != K_FN) trap(site, "find takes a function");
  for (int i = 0; i < self.a->len; i++)
    if (call(a[0].fn, NUL, &self.a->v[i], 1, site).i) return self.a->v[i];
  return NUL;
}

static Val bi_equals(Val self, Val *a, int n, Node *site) {
  if (n != 1 || a[0].k != K_ARR) trap(site, "equals takes an array");
  if (self.a->len != a[0].a->len) return B(0);
  for (int i = 0; i < self.a->len; i++)
    if (!eq(self.a->v[i], a[0].a->v[i])) return B(0);
  return B(1);
}

static Val bi_includes(Val self, Val *a, int n, Node *site) {
  if (n != 1) trap(site, "includes takes a value");
  for (int i = 0; i < self.a->len; i++)
    if (eq(self.a->v[i], a[0])) return B(1);
  return B(0);
}

static Val bi_pop(Val self, Val *a, int n, Node *site) {
  if (!self.a->len) trap(site, "pop of an empty array");
  return self.a->v[--self.a->len];
}

static Val bi_join(Val self, Val *a, int n, Node *site) {
  if (n != 1 || a[0].k != K_STR) trap(site, "join takes a separator");
  Str *s = mkstr("", 0);
  for (int i = 0; i < self.a->len; i++) s = cat(i ? cat(s, a[0].s) : s, tostr(self.a->v[i]));
  return S(s);
}

static void builtin(Mod *m, const char *name, Builtin *bi) {
  Fn *fn = alloc(sizeof(Fn));
  fn->bi = bi;
  global(m, intern(name, strlen(name)), 0, FN(fn));
}

static void init(void) {
  prelude = alloc(sizeof(Mod)), strmethods = alloc(sizeof(Mod)), arrmethods = alloc(sizeof(Mod));
  anytype = newtype(T_ANY, 0), fntype = newtype(T_FN, 0);
  for (int k = 0; k <= K_STR; k++) {
    prims[k] = newtype(T_PRIM, 0);
    prims[k]->prim = k;
    global(prelude, intern(primnames[k], strlen(primnames[k])), 0, TY(prims[k]));
  }
  global(prelude, intern("void", 4), 0, TY(prims[K_NULL]));
  global(prelude, intern("Type", 4), 0, TY(anytype));
  builtin(prelude, "print", bi_print);
  builtin(prelude, "readFile", bi_readFile);
  builtin(prelude, "writeFile", bi_writeFile);
  builtin(prelude, "args", bi_args);
  builtin(prelude, "targetOs", bi_targetOs);
  builtin(prelude, "targetArch", bi_targetArch);
  builtin(prelude, "now", bi_now);
  builtin(prelude, "idOf", bi_idOf);
  builtin(prelude, "chr", bi_chr);
  builtin(prelude, "fromBytes", bi_fromBytes);
  builtin(prelude, "toC", bi_toC);
  builtin(prelude, "fromC", bi_fromC);
  builtin(prelude, "fromCN", bi_fromCN);
  builtin(prelude, "toCN", bi_toCN);
  builtin(prelude, "peek", bi_peek);
  builtin(prelude, "poke", bi_poke);
  mem = alloc(sizeof(Mod));
  builtin(mem, "addr", bi_addr);
  builtin(mem, "load", bi_load);
  builtin(strmethods, "slice", bi_slice);
  builtin(strmethods, "at", bi_at);
  builtin(strmethods, "copy", bi_copy);
  builtin(arrmethods, "slice", bi_slice);
  builtin(arrmethods, "at", bi_at);
  builtin(arrmethods, "push", bi_push);
  builtin(arrmethods, "append", bi_append);
  builtin(arrmethods, "map", bi_map);
  builtin(arrmethods, "filter", bi_filter);
  builtin(arrmethods, "some", bi_some);
  builtin(arrmethods, "every", bi_every);
  builtin(arrmethods, "find", bi_find);
  builtin(arrmethods, "equals", bi_equals);
  builtin(arrmethods, "includes", bi_includes);
  builtin(arrmethods, "pop", bi_pop);
  builtin(arrmethods, "join", bi_join);
  s_length = intern("length", 6), s_toString = intern("toString", 8), s_members = intern("members", 7), s_value = intern("value", 5);
  s_constructor = intern("constructor", 11), s_main = intern("main", 4);
  s_kind = intern("kind", 4), s_name = intern("name", 4), s_fields = intern("fields", 6), s_elem = intern("elem", 4);
  s_tags = intern("tags", 4), s_base = intern("base", 4), s_args = intern("args", 4);
  s_field[0] = s_name, s_field[1] = intern("type", 4), s_field[2] = intern("readonly", 8), s_field[3] = s_tags;
  kwinit();
}

static void fatal(Node *n, const char *msg, const char *arg) {
  if (n) fprintf(stderr, "%s:%d:%d: ", n->file, n->line, n->col);
  fprintf(stderr, "%s%s\n", msg, arg);
  exit(1);
}

static Mod *module(Mod *m, Str *src, int header);
static char *rooted(const char *path);

static Mod *load(const char *path, Node *site) {
  char *real = realpath(path, 0);
  if (!real) fatal(site, "cannot open ", path);
  if (!strcmp(real, rooted("/builtin/mem.bob"))) return mem;  // its intrinsics have no body
  Mod **tail = &mods;
  for (; *tail; tail = &(*tail)->next) if (!strcmp((*tail)->path, real)) return *tail;
  Mod *m = *tail = alloc(sizeof(Mod));
  m->path = real;
  return module(m, slurp(real), 0);
}

/// path within the checkout the interpreter is built in, the parent of its executable's directory.
static char *rooted(const char *path) {
  static char root[PATH_MAX];
  if (!*root) {
#ifdef __APPLE__
    char exe[PATH_MAX];
    uint32_t n = sizeof exe;
    _NSGetExecutablePath(exe, &n);
    realpath(exe, root);
#else
    realpath("/proc/self/exe", root);
#endif
    *strrchr(root, '/') = 0, *strrchr(root, '/') = 0;
  }
  char *p = alloc(strlen(root) + strlen(path) + 1);
  return strcat(strcpy(p, root), path);
}

static int samedir(const char *a, const char *b) {
  long n = strrchr(a, '/') - a;
  return n == strrchr(b, '/') - b && !memcmp(a, b, n);
}

/// Compiles and loads the C that defines the static functions of header module m, which s imports, if it has any.
static void glue(Mod *m, Node *s, Mod *bind, Val includes) {
  Arr *syms = mkarr(0);
  for (int i = 0; i < m->prog->n; i++)
    if (m->prog->v[i]->k == N_FN) bi_push(A(syms), (Val[]){S(tstr(m->prog->v[i]->sym->s))}, 1, s);
  const char *slash = strrchr(s->file, '/');
  Str *path = cat(mkstr(s->file, slash ? slash + 1 - s->file : 0), s->val.s);
  Str *c = call(gval(lookup(bind, intern("glue", 4))).fn, NUL, (Val[]){S(path), A(syms)}, 2, s).s;
  if (!c->len) return;
  Arr *dirs = call(gval(lookup(bind, intern("includesOf", 10))).fn, NUL, (Val[]){S(tstr(s->file)), includes}, 2, s).a;
  char dir[] = "/tmp/boberXXXXXX", src[64], so[64];
  if (!mkdtemp(dir)) trap(s, "cannot make %s", dir);
  snprintf(src, sizeof src, "%s/glue.c", dir), snprintf(so, sizeof so, "%s/glue.so", dir);
  FILE *out = fopen(src, "w");
  if (!out || fwrite(c->p, 1, c->len, out) != (size_t)c->len || fclose(out)) trap(s, "cannot write %s", src);
  Str *cmd = tstr("cc -shared -fPIC -o ");
  cmd = cat(cmd, tstr(so)), cmd = cat(cmd, tstr(" ")), cmd = cat(cmd, tstr(src));
  for (int i = 0; i < dirs->len; i++) cmd = cat(cat(cmd, tstr(" -I")), dirs->v[i].s);
  int failed = system(cstr(S(cmd), s)) || !dlopen(so, RTLD_NOW | RTLD_GLOBAL);
  remove(src), remove(so), rmdir(dir);  // the library stays mapped
  if (failed) trap(s, "cannot compile the C of %.*s", path->len, path->p);
}

static Mod *header(Node *s) {
  static Mod *bindm;
  static Fn *bind;
  static Val includes;
  if (!bind) {
    bind = gval(lookup(bindm = load(rooted("/src/c/bind.bob"), s), intern("bind", 4))).fn;
    includes = A(mkarr(0));
    for (int i = 3; i < argc_; i++) {  // the program's -I flags, as a C compiler takes them
      char *a = argv_[i], *d = strncmp(a, "-I", 2) ? 0 : a[2] ? a + 2 : i + 1 < argc_ ? argv_[++i] : 0;
      if (d) bi_push(includes, (Val[]){S(tstr(d))}, 1, s);
    }
  }
  Mod *m = alloc(sizeof(Mod));
  m->path = cstr(S(s->val.s), s);
  Arr *names = mkarr(0);
  for (Mod *x = mods; x; x = x->next)
    for (int i = 0; i < x->prog->n; i++) {
      Node *t = x->prog->v[i];
      if (t->k != N_IMPORT || t->cmod || !eq(S(t->val.s), S(s->val.s)) || !samedir(t->file, s->file)) continue;
      t->cmod = m;
      for (int j = 0; j < t->n; j++) bi_push(A(names), (Val[]){S(mkstr(t->v[j]->sym->s, t->v[j]->sym->len))}, 1, s);
    }
  Val args[] = {S(mkstr(s->file, strlen(s->file))), S(s->val.s), A(names), includes};
  Handler h = {handlers, .depth = depth};
  handlers = &h;
  if (setjmp(h.jb)) {
    handlers = h.up, depth = h.depth;
    Str *e = tostr(thrown);
    trap(s, "%.*s", e->len, e->p);
  }
  Val src = call(bind, NUL, args, 4, s);
  handlers = h.up;
  module(m, src.s, 1);
  glue(m, s, bindm, includes);
  return m;
}

static Val cimport(Node *imp) {
  Node *s = imp->a;
  if (!s->cmod) header(s);
  else if (!s->cmod->prog) trap(imp, "binding %.*s needs %s itself", s->val.s->len, s->val.s->p, imp->sym->s);
  return gval(lookup(s->cmod, imp->sym));
}

static Mod *module(Mod *m, Str *src, int header) {
  Node *prog = m->prog = parse(m->path, src->p, src->len, m, header);
  for (int i = 0; i < prog->n; i++) {
    Node *s = prog->v[i];
    if (s->k == N_VAR && s->c->k == P_ID) global(m, s->c->sym, s, NUL);
    else if (s->k == N_FN || s->k == N_CLASS || s->k == N_IFACE || s->k == N_TYPEDEF) global(m, s->sym, s, NUL);
    else if (s->k != N_IMPORT && s->k != N_TEST) fatal(s, "not allowed at the top level", "");
  }
  for (int i = 0; i < prog->n; i++) {
    Node *s = prog->v[i];
    if (s->k != N_IMPORT) continue;
    char p[PATH_MAX], *slash = strrchr(m->path, '/');
    Str *spec = s->val.s;
    if (spec->len > 2 && spec->p[spec->len - 2] == '.' && strchr("hc", spec->p[spec->len - 1])) {
      if (s->op == '*' || s->as) fatal(s, "a C file's names are imported by name", "");
      for (int j = 0; j < s->n; j++) s->v[j]->a = s, global(m, s->v[j]->as, s->v[j], NUL);
      continue;
    }
    int tk = spec->len >= 4 && !memcmp(spec->p + spec->len - 4, ".bob", 4);
    snprintf(p, sizeof p, "%.*s/%.*s%s", (int)(slash - m->path), m->path, spec->len, spec->p, tk ? "" : ".bob");
    Mod *t = load(p, s);
    if (s->as) global(m, s->as, 0, (Val){K_MOD, .m = t});
    if (s->op == '*') for (int j = 0; j < t->n; j++) add(m, t->names[j], t->g[j]);
    for (int j = 0; j < s->n; j++) {
      Global *g = lookup(t, s->v[j]->sym);
      if (!g) fatal(s->v[j], "no such export: ", s->v[j]->sym->s);
      add(m, s->v[j]->as, g);
    }
  }
  return m;
}

static void *run(void *unused) {
  if (argc_ < 3 || (strcmp(argv_[1], "run") && strcmp(argv_[1], "test"))) {
    fprintf(stderr, "usage: bober run|test <file> [args...]\n");
    exit(2);
  }
  init();
  Mod *m = load(argv_[2], 0);
  if (setjmp(trapjb)) {
    fflush(stdout);
    fprintf(stderr, "%s\n", trapmsg);
    exit(1);
  }
  for (Mod *x = mods; x; x = x->next)
    for (int j = 0; j < x->n; j++) {
      Val v = gval(x->g[j]);
      if (v.k == K_TYPE && v.t->tk == T_CLASS)
        for (int k = 0; k < v.t->cls->statics.n; k++) gval(v.t->cls->statics.g[k]);
    }
  if (!strcmp(argv_[1], "test")) {
    static int i, fails;
    for (i = 0; i < m->prog->n; i++) {
      Node *t = m->prog->v[i];
      if (t->k != N_TEST) continue;
      Str *name = testpath = tostr(eval(t->a, 0));
      handlers = 0, depth = 0, subfails = 0;
      if (setjmp(trapjb)) {
        printf("FAIL %.*s: %s\n", name->len, name->p, trapmsg);
        fails++;
        continue;
      }
      exec(t->b, alloc(fsize(t)));
      if (subfails) printf("FAIL %.*s: failed subtests: %d\n", name->len, name->p, subfails), fails++;
      else printf("ok %.*s\n", name->len, name->p);
    }
    exit(fails ? 1 : 0);
  }
  Global *g = lookup(m, s_main);
  if (!g || gval(g).k != K_FN) fatal(0, m->path, ": no main function");
  Val r = call(g->v.fn, NUL, 0, 0, g->def);
  exit(isint(r.k) ? (int)r.i : 0);
}

long bober_io_run(const char *file, char *const *argv, const char *out);

/// Stands in for src/backend/llvm/lld.cpp by running the system's linker args[0], ld.lld or ld64.lld, since the
/// system's libLLVM has no LLD.
int bober_lld(int n, const char **args) {
  const char *argv[n + 1];
  memcpy(argv, args, n * sizeof *args), argv[n] = 0;
  return bober_io_run(args[0], (char *const *)argv, 0);
}

int main(int argc, char **argv) {
  pthread_attr_t attr;
  pthread_t t;
  GC_INIT();
  argc_ = argc, argv_ = argv;
  setvbuf(stdout, 0, _IOLBF, 0);  // so lines keep their order with writes to fd 1 through src/io.c
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 1L << 30);
  pthread_create(&t, &attr, run, 0);
  pthread_join(t, 0);
  return 0;
}
