//! Type descriptors (rt.h's bober_type): those emit made, which section bober_types lists, completed and found by key, and
//! those a generic's code forms of its type args' at run time, made once each; and the ops on the numbers a union
//! holds that such code does without knowing their type.

#include "rt.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

BOBER_SECTION(const bober_type *const, bober_types);

// Primitive type ids (src/bc/kind.bob) and Type.kind's indices (builtin/type.bob).
enum { NUL = 1, BOOL, I8, I16, I32, I64, U8, U16, U32, U64, F32, F64, STRING, TYPE = 16, INT, FLOAT };
enum { KLITERAL = 5, KARRAY, KFIXED, KTUPLE, KOBJECT, KCLASS, KINTERFACE, KFUNCTION, KASYNC, KUNION, KTYPE, KPARAM };
enum { REF = -1, ANY = -2, PLACE = -3, PACKED = -4 };
static const char *const ops[] = {"Eq", "Ne", "Lt", "Gt", "Le", "Ge", "Add", "Sub", "Mul", "Div", "Rem", "Pow", "And", "Or", "Xor", "Shl", "Shr", "Neg"};

static void *alloc(size_t n) {
  void *p = calloc(1, n);
  if (!p) bober_trap("out of memory", 0, -1);
  return p;
}

static int64_t len(bober_string s) { return (s.head & 0xff) == 0xff ? (int64_t)(s.head >> 8) : 0; }

static bober_string string(const char *p, int64_t n) {
  char *b = alloc(n + 1);
  memcpy(b, p, n);
  return (bober_string){(uint64_t)n << 8 | 0xff, b, 0};
}

// The descriptors by key, a table of open addressing.

static const bober_type **table;
static uint64_t cap, count;

static uint64_t hash(const char *p, int64_t n) {
  uint64_t h = 0xcbf29ce484222325;
  for (int64_t i = 0; i < n; i++) h = (h ^ (uint8_t)p[i]) * 0x100000001b3;
  return h;
}

/// The slot of key p, n bytes long: its descriptor's or the empty one it would go in.
static const bober_type **slot(const char *p, int64_t n) {
  for (uint64_t i = hash(p, n) & (cap - 1);; i = (i + 1) & (cap - 1)) {
    const bober_type *t = table[i];
    if (!t || (len(t->key) == n && !memcmp(t->key.bytes, p, n))) return &table[i];
  }
}

static void complete(bober_type *t);

static void add(const bober_type *t) {
  if (2 * (count + 1) > cap) {
    const bober_type **old = table;
    uint64_t n = cap;
    cap = cap ? 2 * cap : 1024;
    table = alloc(cap * sizeof *table);
    count = 0;
    for (uint64_t i = 0; i < n; i++)
      if (old[i]) add(old[i]);
    free(old);
  }
  const bober_type **s = slot(t->key.bytes, len(t->key));
  if (!*s) *s = t, count++;
}

/// The descriptor of key, completing those emit made on first use.
static const bober_type *find(const char *p, int64_t n) {
  static bool found;
  if (!found) {
    found = true;
    for (const bober_type *const *t = bober_types; t < bober_types_end; t++) add(*t);
    for (const bober_type *const *t = bober_types; t < bober_types_end; t++) complete((bober_type *)*t);
  }
  return cap ? *slot(p, n) : 0;
}

static uint64_t fit(int64_t id, uint64_t bits);

void bober_types_complete(void) { find("", 0); }

// Completion: how a value is held, and the struct it refers to.

/// A layout of size bytes, its refs' bits in words.
static const bober_layout *layout(int64_t size, const uint64_t *words) {
  int64_t n = (size / 8 + 63) / 64;
  bool any = false;
  for (int64_t i = 0; i < n; i++) any = any || words[i];
  if (!any) return 0;
  bober_layout *l = alloc(sizeof *l + n * 8);
  l->size = size;
  memcpy(l->refs, words, n * 8);
  return l;
}

/// Sets in words the bits of the refs of layout l at offset at, n times every stride bytes.
static void mark(uint64_t *words, const bober_layout *l, int64_t at, int64_t n, int64_t stride) {
  for (int64_t k = 0; l && k < n; k++)
    for (int64_t i = 0; i < l->size / 8; i++)
      if (l->refs[i / 64] >> (i % 64) & 1) {
        int64_t w = (at + k * stride) / 8 + i;
        words[w / 64] |= (uint64_t)1 << (w % 64);
      }
}

static int64_t up(int64_t n, int64_t align) { return (n + align - 1) / align * align; }

static const bober_type *form(const bober_type *t, int64_t n, const bober_type *const *params, const bober_type *const *args, bool held);

static bool inPlace(const bober_field *f) { return f->mods & 16 && f->type->kind == KCLASS; }

/// The byte of a value of t that it does not use, else -1: a ref's last, a string's ref's (§4.1).
static int64_t spare(const bober_type *t) { return t->held == REF ? 7 : t->held == STRING ? 23 : -1; }

/// Lays out union t, its members completed, as src/emit.bob's shape does (§4.4): packed, each member at offset 0, its
/// tag's byte in length, -1 if its members are null and a ref, which null is the zero of. False if a member is held
/// as any, as t is then.
static bool pack(bober_type *t) {
  const bober_type *widest = t->parts[0];
  int64_t size = 0, align = 1;  // set last, as complete stops at a type with its align set
  for (int64_t i = 0; i < t->nparts; i++) {
    bober_type *m = (bober_type *)t->parts[i];
    complete(m);
    if (m->held == ANY) return false;
    if (m->size > size) size = m->size, widest = m;
    if (m->align > align) align = m->align;
  }
  int64_t k = spare(widest);
  bool shared = k >= 0;
  for (int64_t i = 0; i < t->nparts; i++) shared = shared && (t->parts[i]->size <= k || spare(t->parts[i]) == k);
  if (t->nparts == 2 && (t->parts[0]->id == NUL || t->parts[1]->id == NUL) && k == 7) t->length = -1;
  else if (shared) t->length = k;
  else t->length = size, size = up(size + 1, align);
  // Each word a ref if any member's is, as the collector may take a member's number for a ref.
  uint64_t *words = alloc((size / 512 + 1) * 8);
  for (int64_t i = 0; i < t->nparts; i++) mark(words, t->parts[i]->layout, 0, 1, 0);
  t->held = PACKED, t->size = size, t->align = align, t->layout = layout(size, words);
  free(words);
  return true;
}

/// The index of the member union t's zero is of: null if one, else its first (§5.1).
static int64_t zeroOf(const bober_type *t) {
  for (int64_t i = 0; i < t->nparts; i++)
    if (t->parts[i]->id == NUL) return i;
  return 0;
}

/// The member of packed union t whose value is at p: its tag byte counts from the one its zero is of.
static int64_t member(const bober_type *t, const void *p) {
  if (t->length < 0) return *(void *const *)p ? !zeroOf(t) : zeroOf(t);
  return (((const uint8_t *)p)[t->length] + zeroOf(t)) % t->nparts;
}

/// The alignment of the struct a value of t, completed, refers to: its fields' greatest.
static int64_t salign(const bober_type *t) {
  int64_t a = 1;
  for (int64_t i = 0; i < t->nfields; i++) {
    int64_t b = inPlace(&t->fields[i]) ? salign(t->fields[i].type) : t->fields[i].type->align;
    a = b > a ? b : a;
  }
  return a;
}

static void complete(bober_type *t) {
  if (t->align) return;
  if (!t->base) t->base = t;
  // The type a value of t is held and tagged as, first, as emit reads it from a descriptor formed at run time.
  if (!t->canon) t->canon = form(t, 0, 0, 0, true);
  const bober_type *c = t->canon;
  // A ref, and a string or union, whose third word is one.
  static const struct { int64_t size; uint64_t refs[1]; } ref = {8, {1}}, str = {24, {4}};
  if (t->id) {
    static const int8_t sizes[] = {0, 1, 1, 1, 2, 4, 8, 1, 2, 4, 8, 4, 8, 24, 24, 24, 8, 8, 8};
    t->held = t->id == INT ? I64 : t->id == FLOAT ? F64 : t->id;
    t->size = t->align = sizes[t->held];
    if (t->held == STRING) t->align = 8, t->layout = (const bober_layout *)&str;
  } else if (c != t && (c->kind != t->kind || t->kind == KUNION)) {  // a literal, or a union with them, held as its canon
    complete((bober_type *)c);
    t->held = c->held, t->size = c->size, t->align = c->align, t->layout = c->layout;
  } else if (t->kind == KFIXED) {
    bober_type *e = (bober_type *)t->elem;
    complete(e);
    t->held = PLACE, t->size = e->size * t->length, t->align = e->align;
    uint64_t *words = alloc((t->size / 512 + 1) * 8);
    mark(words, e->layout, 0, t->length, e->size);
    t->layout = layout(t->size, words);
    free(words);
  } else if (t->kind == KUNION && t->nparts && pack(t)) {
  } else if (t->kind == KOBJECT || t->kind == KINTERFACE || t->kind == KUNION) {
    t->held = ANY, t->size = 24, t->align = 8, t->layout = (const bober_layout *)&str;
  } else {
    t->held = REF, t->size = t->align = 8, t->layout = (const bober_layout *)&ref;
  }
  if (t->kind != KCLASS && t->kind != KOBJECT && t->kind != KTUPLE) return;
  // The struct, its fields in order each at the next offset its alignment allows, an embedded one in place.
  int64_t at = 0, align = 1;
  for (int64_t i = 0; i < t->nfields; i++) {
    bober_field *f = &t->fields[i];
    bober_type *ft = (bober_type *)f->type;
    complete(ft);
    int64_t a = inPlace(f) ? salign(ft) : ft->align, n = inPlace(f) ? ft->ssize : ft->size;
    at = up(at, a), align = a > align ? a : align;
    f->offset = at, at += n;
  }
  t->ssize = up(at, align);
  uint64_t *words = alloc((t->ssize / 512 + 1) * 8);
  for (int64_t i = 0; i < t->nfields; i++) {
    const bober_field *f = &t->fields[i];
    mark(words, inPlace(f) ? f->type->slayout : f->type->layout, f->offset, 1, 0);
  }
  t->slayout = layout(t->ssize, words);
  free(words);
}

// Types formed at run time.

/// Appends the n bytes at p to the buffer b of *size, *cap.
static void put(char **b, int64_t *size, int64_t *cap, const char *p, int64_t n) {
  if (*size + n > *cap) *cap = 2 * (*size + n), *b = realloc(*b, *cap);
  memcpy(*b + *size, p, n), *size += n;
}

/// Orders types by hash, which no two share (src/bc/program.bob's headOf), as its union does.
static int byHash(const void *a, const void *b) {
  uint64_t x = (*(const bober_type *const *)a)->hash, y = (*(const bober_type *const *)b)->hash;
  return (x > y) - (x < y);
}

static uint64_t mix(uint64_t h, uint64_t x) { return (h ^ x) * 0x100000001b3; }

/// The hash of a list of the n types at ts, as src/bc/program.bob's.
static uint64_t list(const bober_type *const *ts, int64_t n) {
  uint64_t h = mix(0xcbf29ce484222325, n);
  for (int64_t i = 0; i < n; i++) h = mix(h, ts[i]->hash);
  return h;
}

/// t's key, as src/emit.bob's key makes it: its shape's up to its ( or [, then its parts', fields' and elem's.
static bober_string keyOf(const bober_type *t) {
  int64_t size = 0, room = 64;
  char *b = alloc(room);
  const char *k = t->key.bytes;
  int64_t open = 0;
  while (open < len(t->key) && k[open] != '(' && k[open] != '[') open++;
  bool nominal = t->kind == KCLASS || t->kind == KINTERFACE;
  put(&b, &size, &room, k, open);
  put(&b, &size, &room, nominal ? "[" : "(", 1);
  for (int64_t i = 0; i < t->nparts; i++) {
    if (i) put(&b, &size, &room, ",", 1);
    put(&b, &size, &room, t->parts[i]->key.bytes, len(t->parts[i]->key));
  }
  for (int64_t i = 0; !nominal && i < t->nfields; i++) {
    const bober_field *f = &t->fields[i];
    if (i) put(&b, &size, &room, ",", 1);
    if (t->kind == KOBJECT) {  // its mods, if any, before its name, which starts with no digit
      char m[24];
      if (f->mods) put(&b, &size, &room, m, snprintf(m, sizeof m, "%lld", (long long)f->mods));
      put(&b, &size, &room, f->name.bytes, len(f->name)), put(&b, &size, &room, ":", 1);
    }
    put(&b, &size, &room, f->type->key.bytes, len(f->type->key));
  }
  if (t->elem) {
    put(&b, &size, &room, t->elem->key.bytes, len(t->elem->key));
    char n[24];
    put(&b, &size, &room, n, snprintf(n, sizeof n, ",%lld", (long long)t->length));
  }
  put(&b, &size, &room, nominal ? "]" : ")", 1);
  bober_string s = string(b, size);
  free(b);
  return s;
}

/// r's hash, as src/bc/program.bob's of its head and parts.
static uint64_t hashOf(const bober_type *r) {
  if (r->elem) return mix(r->head, r->elem->hash);
  if (r->kind == KFUNCTION) return mix(mix(r->head, list(r->parts, r->length)), r->parts[r->length]->hash);
  if (r->kind != KTUPLE && r->kind != KOBJECT) return mix(r->head, list(r->parts, r->nparts));
  uint64_t h = mix(0xcbf29ce484222325, r->nfields);
  for (int64_t i = 0; i < r->nfields; i++) h = mix(h, r->fields[i].type->hash);
  return mix(r->head, h);
}

/// Generic code forms its types each time it runs, so the last ones formed are kept by t and args, which fix params.
const bober_type *bober_type_of(const bober_type *t, int64_t n, const bober_type *const *params, const bober_type *const *args) {
  static struct formed { const bober_type *t, *args[4], *made; int64_t n; } formed[4096];
  if (n > 4) return form(t, n, params, args, false);
  uint64_t h = mix(0, (uint64_t)t);
  for (int64_t i = 0; i < n; i++) h = mix(h, (uint64_t)args[i]);
  struct formed *f = &formed[h >> 52];
  if (f->t != t || f->n != n || memcmp(f->args, args, n * sizeof *args)) {
    *f = (struct formed){t, .made = form(t, n, params, args, false), .n = n};
    memcpy(f->args, args, n * sizeof *args);
  }
  return f->made;
}

/// t with each of the n params in it as the arg at its index, and if held, each literal as its base.
static const bober_type *form(const bober_type *t, int64_t n, const bober_type *const *params, const bober_type *const *args, bool held) {
  if (t->kind == KPARAM) {
    for (int64_t i = 0; i < n; i++)
      if (params[i] == t) return args[i];
    return t;
  }
  if (held && t->canon) return t->canon;  // emit's of a closed type, so a literal's, or complete's
  bool nominal = t->kind == KCLASS || t->kind == KINTERFACE;
  if (t->id || t->kind == KLITERAL || (nominal && (held || !t->nparts))) return t;  // a class is held with its args
  bober_type *r = alloc(sizeof *r);
  *r = *t;
  r->held = r->size = r->align = r->ssize = 0, r->layout = r->slayout = 0, r->canon = held ? r : 0;
  if (t->elem) r->elem = form(t->elem, n, params, args, held);
  int64_t k = 0, room = t->nparts + 1;
  const bober_type **parts = alloc(room * sizeof *parts);
  for (int64_t i = 0; i < t->nparts; i++) {
    // A union's members flattened, each once, in the order of their hashes, as src/bc/program.bob's union.
    const bober_type *p = form(t->parts[i], n, params, args, held);
    for (int64_t j = 0; j < (t->kind == KUNION && p->kind == KUNION ? p->nparts : 1); j++) {
      if (k == room) parts = realloc(parts, (room *= 2) * sizeof *parts);
      parts[k++] = t->kind == KUNION && p->kind == KUNION ? p->parts[j] : p;
    }
  }
  if (t->kind == KUNION) {
    qsort(parts, k, sizeof *parts, byHash);
    int64_t u = 0;
    for (int64_t i = 0; i < k; i++)
      if (!u || parts[u - 1] != parts[i]) parts[u++] = parts[i];
    if (u == 1) {
      const bober_type *p = parts[0];
      free(parts), free(r);
      return p;
    }
    k = u;
  }
  r->parts = parts, r->nparts = k;
  bober_field *fields = alloc((t->nfields + 1) * sizeof *fields);
  memcpy(fields, t->fields, t->nfields * sizeof *fields);
  r->fields = fields;
  // A class's key is of its args, so that one whose fields hold it is found while they are formed.
  for (int64_t i = 0; !nominal && i < t->nfields; i++) fields[i].type = form(fields[i].type, n, params, args, held);
  r->hash = hashOf(r);
  r->key = keyOf(r);
  const bober_type *have = find(r->key.bytes, len(r->key));
  if (have) {
    free((void *)r->key.bytes), free(fields), free(parts), free(r);
    return have;
  }
  // Added first, so that its canon, and a class's fields that hold it, find it.
  if (nominal) r->base = t->base ? t->base : t;
  add(r);
  for (int64_t i = 0; nominal && i < t->nfields; i++) fields[i].type = form(fields[i].type, n, params, args, held);
  complete(r);
  return r;
}

int64_t bober_field_of(const bober_type *t, const char *name, int64_t n, const bober_type **type) {
  for (int64_t i = 0; i < t->nfields; i++) {
    const bober_field *f = &t->fields[i];
    if (len(f->name) == n && !memcmp(f->name.bytes, name, n)) return *type = f->type, f->offset;
  }
  for (int64_t i = 0; i < t->nfields; i++) {
    const bober_field *f = &t->fields[i];
    int64_t at = f->mods & 16 && f->type->kind == KCLASS ? bober_field_of(f->type, name, n, type) : -1;
    if (at >= 0) return f->offset + at;
  }
  return -1;
}

void *bober_method_of(const bober_type *t, const char *name, int64_t n) {
  for (int64_t i = 0; i < t->nmethods; i++)
    if (len(t->methods[i].name) == n && !memcmp(t->methods[i].name.bytes, name, n)) return t->methods[i].fn;
  return 0;
}

// Values of a type known at run time.

int64_t bober_load(const bober_type *t, const void *p, bober_union *u) {
  t = t->canon;  // packed, if a union, as emit packs it, its literals their base
  if (t->held == ANY) return *u = *(const bober_union *)p, 0;
  if (t->held == PACKED) {
    int64_t n = bober_load(t->parts[member(t, p)], p, u);
    u->ref = (void *)((uintptr_t)u->ref << 8 >> 8);  // without the tag in its top byte; a string's emit's load clears
    return n;
  }
  *u = (bober_union){t->canon, 0, 0};
  if (t->held == STRING || t->held == PLACE) return t->size;
  if (t->held == REF) u->ref = *(void *const *)p;
  else memcpy(&u->bits, p, t->size), u->bits = fit(t->held, u->bits);
  return 0;
}

void bober_store(const bober_type *t, void *p, const bober_union *u) {
  t = t->canon;
  bober_written(p);
  if (t->held == PACKED) {
    int64_t i = 0;
    while (i < t->nparts - 1 && !bober_is(t->parts[i], u)) i++;
    memset(p, 0, t->size);
    bober_store(t->parts[i], p, u);
    if (t->length >= 0) ((uint8_t *)p)[t->length] = (i - zeroOf(t) + t->nparts) % t->nparts;
  } else if (t->held == ANY) *(bober_union *)p = *u;
  else if (t->held == STRING || t->held == PLACE) memcpy(p, u->ref, t->size);
  else if (t->held == REF) *(void **)p = u->ref;
  else memcpy(p, &u->bits, t->size);
}

void bober_zero(const bober_type *t, bober_union *u) {
  *u = (bober_union){t = t ? t->canon : t, 0, 0};
  if (!t) return;
  if (t->kind == KUNION) {
    u->tag = t->parts[zeroOf(t)];  // as emit's zero
  } else if (t->held == STRING || t->held == PLACE) {
    u->ref = bober_alloc(t->size, t->layout);
  }
}

static bool has(const bober_type *c, bober_string name) {
  const bober_type *type;
  return bober_method_of(c, name.bytes, len(name)) || bober_field_of(c, name.bytes, len(name), &type) >= 0;
}

int64_t bober_is(const bober_type *t, const bober_union *u) {
  for (int64_t i = 0; t->kind == KUNION && i < t->nparts; i++)
    if (bober_is(t->parts[i], u)) return 1;
  if (t->kind == KUNION) return 0;
  if (t->kind == KINTERFACE) {  // which a class or object fits by having its members, which methods names (§4.7)
    const bober_type *c = u->tag;
    if (c->kind != KCLASS && c->kind != KOBJECT) return 0;
    for (int64_t i = 0; i < t->nmethods; i++)
      if (!has(c, t->methods[i].name)) return 0;
    return 1;
  }
  if (u->tag != t->canon) return 0;
  if (t->kind != KLITERAL) return 1;
  if (t->held != STRING) return u->bits == (uint64_t)t->length;
  const bober_string *s = u->ref;
  int64_t n = bober_length(u);
  const char *p = (s->head & 0xff) == 0xff ? (const char *)s->ref + (uintptr_t)s->bytes : (const char *)s + 1;
  return n == len(t->value) && !memcmp(p, t->value.bytes, n);
}

int64_t bober_length(const bober_union *u) {
  if (u->tag->kind == KFIXED) return u->tag->length;
  uint64_t h = *(const uint64_t *)u->ref;  // a string's head, its length in byte 0 if short, or an array's
  return u->tag->held != STRING ? (int64_t)h : (h & 0xff) == 0xff ? (int64_t)(h >> 8) : (int64_t)(h & 0xff);
}

void *bober_element(const bober_union *u, int64_t i, const bober_type **type, const char *file, int32_t at) {
  if (i < 0 || i >= bober_length(u)) bober_trap("index out of range", file, at);
  const bober_type *e = *type = u->tag->elem;
  return (char *)(u->tag->kind == KFIXED ? u->ref : *(void **)((char *)u->ref + 16)) + i * e->size;
}

// Numbers.

static int width(int64_t id) { return id == I8 || id == U8 ? 8 : id == I16 || id == U16 ? 16 : id == I32 || id == U32 || id == F32 ? 32 : id == BOOL || id == NUL ? 1 : 64; }
static bool sign(int64_t id) { return id == I8 || id == I16 || id == I32 || id == I64 || id == INT; }
static bool real(int64_t id) { return id == F32 || id == F64 || id == FLOAT; }
static bool number(int64_t id) { return (id >= BOOL && id <= F64) || id == INT || id == FLOAT; }

/// bits truncated to the width of id and extended as a union holds them.
static uint64_t fit(int64_t id, uint64_t bits) {
  int w = width(id);
  if (w == 64) return bits;
  uint64_t m = ((uint64_t)1 << w) - 1;
  return sign(id) && bits >> (w - 1) & 1 ? bits | ~m : bits & m;
}

static double toDouble(int64_t id, uint64_t bits) {
  if (id == F32) {
    float f;
    uint32_t b = bits;
    memcpy(&f, &b, 4);
    return f;
  }
  double d;
  memcpy(&d, &bits, 8);
  return d;
}

static uint64_t fromDouble(int64_t id, double d) {
  if (id == F32) {
    float f = d;
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
  }
  uint64_t b;
  memcpy(&b, &d, 8);
  return b;
}

uint64_t bober_number(int64_t op, const bober_type *t, uint64_t a, uint64_t b, const char *file, int32_t at) {
  int64_t id = t->id;
  const char *o = ops[op];
  if (real(id)) {
    double x = toDouble(id, a), y = toDouble(id, b);
    switch (op) {
    case 0: return x == y;
    case 1: return x != y;
    case 2: return x < y;
    case 3: return x > y;
    case 4: return x <= y;
    case 5: return x >= y;
    case 6: return fromDouble(id, x + y);
    case 7: return fromDouble(id, x - y);
    case 8: return fromDouble(id, x * y);
    case 9: return fromDouble(id, x / y);
    case 10: return fromDouble(id, fmod(x, y));
    case 11: return fromDouble(id, pow(x, y));
    case 17: return fromDouble(id, -x);
    }
    bober_trap(o, file, at);
  }
  bool s = sign(id);
  int w = width(id);
  switch (op) {
  case 0: return a == b;
  case 1: return a != b;
  case 2: return s ? (int64_t)a < (int64_t)b : a < b;
  case 3: return s ? (int64_t)a > (int64_t)b : a > b;
  case 4: return s ? (int64_t)a <= (int64_t)b : a <= b;
  case 5: return s ? (int64_t)a >= (int64_t)b : a >= b;
  case 6: return fit(id, a + b);
  case 7: return fit(id, a - b);
  case 8: return fit(id, a * b);
  case 9:
  case 10:
    if (!b) bober_trap("division by zero", file, at);
    if (s && (int64_t)b == -1) return op == 9 ? fit(id, -a) : 0;
    return fit(id, s ? (uint64_t)(op == 9 ? (int64_t)a / (int64_t)b : (int64_t)a % (int64_t)b) : op == 9 ? a / b : a % b);
  case 11: {
    if (s && (int64_t)b < 0) bober_trap("negative exponent", file, at);
    uint64_t r = 1;
    for (uint64_t x = a, e = b; e; e >>= 1, x *= x)
      if (e & 1) r *= x;
    return fit(id, r);
  }
  case 12: return a & b;
  case 13: return a | b;
  case 14: return a ^ b;
  case 15: return fit(id, a << (b & (w - 1)));
  case 16: return fit(id, s ? (uint64_t)((int64_t)a >> (b & (w - 1))) : (a & (w == 64 ? ~(uint64_t)0 : ((uint64_t)1 << w) - 1)) >> (b & (w - 1)));
  case 17: return fit(id, -a);
  }
  bober_trap(o, file, at);
}

uint64_t bober_convert(const bober_type *to, const bober_type *from, uint64_t bits) {
  int64_t t = to->id, f = from->id;
  if (!number(t) || !number(f)) return bits;
  if (t == BOOL) return real(f) ? toDouble(f, bits) != 0 : bits != 0;
  if (real(f) && real(t)) return fromDouble(t, toDouble(f, bits));
  if (real(t)) return fromDouble(t, sign(f) ? (double)(int64_t)bits : (double)bits);
  if (real(f)) {
    // Saturating, NaN to 0, as LLVM's fptosi.sat.
    double d = toDouble(f, bits);
    int w = width(t);
    if (d != d) return 0;
    if (sign(t)) {
      double hi = ldexp(1, w - 1);
      return fit(t, d >= hi ? (uint64_t)hi - 1 : d < -hi ? (uint64_t)(int64_t)-hi : (uint64_t)(int64_t)d);
    }
    double hi = ldexp(1, w);
    return d >= hi ? fit(t, ~(uint64_t)0) : d <= 0 ? 0 : (uint64_t)d;
  }
  return fit(t, bits);
}
