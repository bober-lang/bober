%{
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#define YYLTYPE int
#define YYLLOC_DEFAULT(C, R, N) ((C) = (N) ? YYRHSLOC(R, 1) : YYRHSLOC(R, 0))
%}

%define parse.error detailed
%locations
%union { Node *n; Sym *s; int i; }

%code {
const char *curfile;
static Mod *curmod;
static int cheader;
static Node *program;
static int pos, nbinds, nctx;
typedef struct { Sym *s; int level, slot; Node *type; } Bind;
static Bind binds[8192];
static struct { Node *fn; int nbinds; } ctx[256];

int yylex(void);
void yyerror(const char *msg);

Node *mk(int k, int tok) {
  Node *n = alloc(sizeof(Node));
  n->k = k, n->line = toks[tok].line, n->col = toks[tok].col, n->file = curfile, n->mod = curmod;
  return n;
}

static Node *node(int k, int tok, Node *a, Node *b, Node *c) {
  Node *n = mk(k, tok);
  n->a = a, n->b = b, n->c = c;
  return n;
}

/// Takes s as the contextual keyword w (lex.bob's Contextual), which it must spell.
static void word(Sym *s, const char *w) {
  char msg[64];
  snprintf(msg, sizeof msg, "syntax error, expected %s", w);
  if (strcmp(s->s, w)) yyerror(msg);
}

/// The flags of the modifier s: static, private or readonly.
static int modifier(Sym *s) {
  if (strcmp(s->s, "private") && strcmp(s->s, "readonly")) word(s, "static");
  return strcmp(s->s, "static") ? 0 : F_STATIC;
}

static Sym *base(Sym *s) {
  int e = s->len >= 4 && !memcmp(s->s + s->len - 4, ".bob", 4) ? s->len - 4 : s->len, b = e;
  while (b > 0 && s->s[b - 1] != '/') b--;
  int ok = b < e && !isdigit(s->s[b]);
  for (int i = b; i < e; i++) ok &= isalnum(s->s[i]) || s->s[i] == '_' || s->s[i] == '$';
  Sym *n = intern(s->s + b, e - b);
  if (!ok || n->tok) yyerror("the last path segment is not an identifier");
  return n;
}

static Node *push(Node *list, Node *x) {
  if (!(list->n & (list->n - 1))) {
    Node **v = alloc(2 * (list->n + 1) * sizeof(Node *));
    for (int i = 0; i < list->n; i++) v[i] = list->v[i];
    list->v = v;
  }
  list->v[list->n++] = x;
  list->flags |= x->flags & F_SPREAD;
  return list;
}

static Node *lit(int tok, Val v) {
  Node *n = mk(N_LIT, tok);
  n->val = v;
  return n;
}

static int bind(Sym *s, int slot) {
  if (nbinds == sizeof binds / sizeof *binds) yyerror("too many names in scope");
  binds[nbinds++] = (Bind){s, nctx - 1, slot};
  return slot;
}

static int declare(Sym *s) {
  return nctx ? bind(s, ctx[nctx - 1].fn->nslots++) : -1;
}

/// A function's type parameter has a slot, which a call with type arguments sets, else any. A class's has one in the class's frame, which an instantiation sets, else its default def. Its own come
/// first, right after it opens; a nested interface's or typedef's follow its name.
static void tparam(Sym *s, Node *def) {
  Node *fn = nctx ? ctx[nctx - 1].fn : 0;
  if (fn && (fn->k == N_FN || fn->k == N_CLASS) && fn->nslots == fn->ntp) {
    Node **v = alloc((fn->ntp + 1) * sizeof(Node *));
    for (int i = 0; i < fn->ntp; i++) v[i] = fn->tdefs[i];
    v[fn->ntp] = def, fn->tdefs = v;
    fn->ntp++, declare(s);
  } else bind(s, -1);
}

static void typed(Node *pattern, Node *type) {
  if (pattern->k == P_ID && pattern->slot >= 0) binds[nbinds - 1].type = type;
}

static Node *ident(Sym *s, int tok) {
  for (int i = nbinds; i-- > 0;)
    if (binds[i].s == s) {
      if (binds[i].slot < 0) return lit(tok, (Val){K_TYPE, .t = anytype});
      Node *n = mk(N_LOCAL, tok);
      n->depth = nctx - 1 - binds[i].level, n->slot = binds[i].slot, n->a = binds[i].type;
      return n;
    }
  Node *n = mk(N_GLOBAL, tok);
  n->sym = s;
  return n;
}

static Node *pid(Sym *s, int tok) {
  Node *n = mk(P_ID, tok);
  n->sym = s, n->slot = declare(s);
  return n;
}

static void hoist(int i) {
  for (int d = 1; d; i++) {
    int t = toks[i].t;
    d += t == '{' || t == LBLOCK ? 1 : t == '}' ? -1 : 0;
    if (d == 1 && (t == FUNCTION || t == CLASS) && nctx) toks[i + 1].op = declare(toks[i + 1].s) + 1;
  }
}

/// Opens the frame of n, a function or a loop, whose iterations each have their own (5.1).
static Node *scope(Node *n) {
  if (nctx == sizeof ctx / sizeof *ctx) yyerror("functions nested too deeply");
  ctx[nctx].fn = n, ctx[nctx++].nbinds = nbinds;
  return n;
}

/// Closes the frame of n; one a closure keeps keeps the frames around too.
static Node *unscope(Node *n) {
  nbinds = ctx[--nctx].nbinds;
  if (nctx && n->flags & F_HEAP) ctx[nctx - 1].fn->flags |= F_HEAP;
  return n;
}

static Node *open(int k, Sym *name, int tok) {
  Node *n = mk(k, tok);
  n->sym = name;
  if (name) n->slot = toks[tok + 1].op ? toks[tok + 1].op - 1 : declare(name);
  if (nctx) ctx[nctx - 1].fn->flags |= F_HEAP;
  return scope(n);
}

static Node *close(Node *fn, Node *list, Node *ret, Node *body) {
  if (list) fn->v = list->v, fn->n = list->n;
  fn->b = ret, fn->c = body;
  return unscope(fn);
}

static Node *bin(int op, Node *a, Node *b, int tok) {
  int k = op == ANDAND ? N_AND : op == OROR ? N_OR : op == QQ ? N_QQ : op == INSTANCEOF ? N_IS : N_BIN;
  Node *n = node(k, tok, a, b, 0);
  n->op = op;
  return n;
}

static Node *cmp(int op, Node *a, Node *b, int tok) {
  Node *n = node(N_CMP, tok, a, b, 0);
  n->op = op;
  if (a->k == N_CMP && !(a->flags & F_PAREN)) n->flags |= F_CHAIN;
  return n;
}

static Node *attach(Node *suffix, Node *a) {
  suffix->a = a;
  suffix->flags |= a->flags & F_OPT;
  return suffix;
}

static Node *prop(int mods, Sym *key, int tok, Node *a, Node *b) {
  Node *n = node(N_PROP, tok, a, b, 0);
  n->sym = key, n->flags = mods;
  return n;
}

static Node *elem(Node *pattern, Node *dflt, int flags, Sym *key) {
  Node *n = node(P_ELEM, 0, 0, dflt, pattern);
  n->line = pattern->line, n->col = pattern->col, n->flags = flags, n->sym = key;
  return n;
}

}

%token <s> IDENT
%token <n> INT FLOAT STR TPL THEAD TMID TEND
%token <i> ASSIGNOP CMP
%token <s> AS ASSERT ASYNC AWAIT BREAK CASE CATCH CLASS CONST CONTINUE DEFAULT ELSE EXPORT FALSE
%token <s> FINALLY FOR FUNCTION IF IMPORT INSTANCEOF INTERFACE LET NEW NUL
%token <s> RETURN SPAWN SWITCH TEST THIS THROW TRUE TRY TYPE TYPEOF WHILE
%token ELLIPSIS QDOT ARROW INC DEC LPA LBLOCK EQ NE LE GE

%left QQ
%left OROR
%left ANDAND
%left CMP INSTANCEOF
%left '|'
%left '^'
%left '&'
%left SHL SHR
%left '+' '-'
%left '*' '/' '%'
%right POW
%precedence AS

%type <n> stmts stmt simple block var pattern pelems pelem pprops pprop params param opttype optinit
%type <n> fndecl arrow body classdecl props prop optbody expr bexpr unary postfix spostfix suffix
%type <n> sprimary primary newcallee template tpl elems elem scases scase ecases ecase labels label
%type <n> ifstmt trystmt catch imps imp name loop tags tpopt
%type <s> key kw
%type <i> mark mods varkw

%%

file: stmts { program = $1; } ;

stmts: %empty { $$ = mk(N_BLOCK, @$); }
  | stmt { $$ = push(mk(N_BLOCK, @1), $1); }
  | stmts ';'
  | stmts ';' stmt { $$ = push($1, $3); }
  ;

mark: %empty { $$ = nbinds; } ;

loop: %empty { $$ = scope(mk(N_FOR, @$)); } ;

block: '{' mark { hoist(@1 + 1); } stmts '}' { nbinds = $2; $$ = $4; } ;

stmt: block | var | fndecl | classdecl | ifstmt | trystmt | simple
  | INTERFACE IDENT { $<i>$ = declare($2); } mark tparams supers tags '{' props '}' {
      nbinds = $4; $$ = $9; $$->k = N_IFACE; $$->sym = $2; $$->slot = $<i>3; }
  | TYPE IDENT { $<i>$ = declare($2); } mark tparams '=' expr {
      nbinds = $4; $$ = node(N_TYPEDEF, @1, $7, 0, 0); $$->sym = $2; $$->slot = $<i>3; }
  | WHILE loop '(' expr ')' block { $$ = unscope($2); $$->k = N_WHILE, $$->a = $4, $$->b = $6; }
  | FOR loop '(' varkw pattern of expr ')' block { $$ = unscope($2); $$->a = $7, $$->c = $5, $$->d = $9; }
  | FOR loop '(' varkw pattern of expr ELLIPSIS expr ')' block {
      $$ = unscope($2); $$->a = $7, $$->b = $9, $$->c = $5, $$->d = $11; }
  | SWITCH '(' expr ')' '{' mark scases '}' { nbinds = $6; $$ = $7; $$->a = $3; }
  | RETURN { $$ = mk(N_RETURN, @1); }
  | RETURN expr { $$ = node(N_RETURN, @1, $2, 0, 0); }
  | THROW expr { $$ = node(N_THROW, @1, $2, 0, 0); }
  | BREAK { $$ = mk(N_BREAK, @1); }
  | CONTINUE { $$ = mk(N_CONTINUE, @1); }
  | ASSERT expr { $$ = node(N_ASSERT, @1, $2, 0, 0); }
  | TEST name { $<n>$ = nctx ? 0 : open(N_TEST, 0, @1); } block {
      $$ = $<n>3 ? close($<n>3, 0, 0, 0) : mk(N_TEST, @1); $$->a = $2; $$->b = $4; }
  | EXPORT stmt { $$ = $2; }
  | IMPORT '{' imps '}' from STR { $$ = $3; $$->val = $6->val; }
  | IMPORT '*' AS IDENT from STR { $$ = mk(N_IMPORT, @1); $$->as = $4; $$->val = $6->val; }
  | IMPORT '*' from STR { $$ = mk(N_IMPORT, @1); $$->op = '*'; $$->val = $4->val; }
  | IMPORT STR { $$ = mk(N_IMPORT, @1); $$->as = base($2->sym); $$->val = $2->val; }
  ;

name: STR | template ;

imps: %empty { $$ = mk(N_IMPORT, @$); }
  | imp { $$ = push(mk(N_IMPORT, @1), $1); }
  | imps ','
  | imps ',' imp { $$ = push($1, $3); }
  ;

imp: IDENT { $$ = mk(N_IMPORT, @1); $$->sym = $1; $$->as = $1; }
  | IDENT AS IDENT { $$ = mk(N_IMPORT, @1); $$->sym = $1; $$->as = $3; }
  ;

simple: spostfix
  | spostfix '=' expr { $$ = node(N_ASSIGN, @2, $1, $3, 0); $$->op = '='; }
  | spostfix ASSIGNOP expr { $$ = node(N_ASSIGN, @2, $1, $3, 0); $$->op = $2; }
  | spostfix INC { $$ = node(N_ASSIGN, @2, $1, lit(@2, (Val){K_INT, .i = 1}), 0); $$->op = '+'; }
  | spostfix DEC { $$ = node(N_ASSIGN, @2, $1, lit(@2, (Val){K_INT, .i = 1}), 0); $$->op = '-'; }
  ;

var: varkw pattern opttype optinit { $$ = node(N_VAR, @1, $3, $4, $2); $$->op = $1; typed($2, $3);
    if (!$4 && cheader) $$->flags |= F_EXTERN; } ;

varkw: LET { $$ = 0; } | CONST { $$ = 1; } ;

opttype: %empty { $$ = 0; } | ':' expr { $$ = $2; } ;

optinit: %empty { $$ = 0; } | '=' expr { $$ = $2; } ;

pattern: IDENT { $$ = pid($1, @1); }
  | '[' pelems ']' { $$ = $2; }
  | '{' pprops '}' { $$ = $2; }
  ;

pelems: %empty { $$ = mk(P_ARR, @$); }
  | pelem { $$ = push(mk(P_ARR, @1), $1); }
  | pelems ','
  | pelems ',' pelem { $$ = push($1, $3); }
  ;

pelem: pattern { $$ = elem($1, 0, 0, 0); }
  | pattern '=' expr { $$ = elem($1, $3, 0, 0); }
  | ELLIPSIS pattern { $$ = elem($2, 0, F_REST, 0); }
  ;

pprops: %empty { $$ = mk(P_OBJ, @$); }
  | pprop { $$ = push(mk(P_OBJ, @1), $1); }
  | pprops ','
  | pprops ',' pprop { $$ = push($1, $3); }
  ;

pprop: key { $$ = elem(pid($1, @1), 0, 0, $1); }
  | key '=' expr { $$ = elem(pid($1, @1), $3, 0, $1); }
  | key ':' pattern { $$ = elem($3, 0, 0, $1); }
  | key ':' pattern '=' expr { $$ = elem($3, $5, 0, $1); }
  | ELLIPSIS IDENT { $$ = elem(pid($2, @2), 0, F_REST, 0); }
  ;

params: %empty { $$ = mk(N_BLOCK, @$); }
  | param { $$ = push(mk(N_BLOCK, @1), $1); }
  | params ','
  | params ',' param { $$ = push($1, $3); }
  ;

param: pattern opttype optinit { $$ = node(N_PARAM, @1, $2, $3, $1); typed($1, $2); }
  | pattern ':' ELLIPSIS expr {  /* a C struct by value */
    $$ = node(N_PARAM, @1, $4, 0, $1); $$->flags = F_EMBED; typed($1, $4); if (!cheader) yyerror("expected a type"); }
  | ELLIPSIS pattern opttype { $$ = node(N_PARAM, @1, $3, 0, $2); $$->flags = F_REST; }
  ;

tparams: %empty | '[' tps ']' ;

tps: %empty | tp | tps ',' | tps ',' tp ;

tp: IDENT tpopt { tparam($1, $2); } ;

tpopt: %empty { $$ = 0; } | extends expr { $$ = 0; } | '=' expr { $$ = $2; } | extends expr '=' expr { $$ = $4; } ;

throws: %empty | IDENT { word($1, "throws"); } tlist ;

tlist: IDENT {} | '|' IDENT {} | tlist '|' IDENT {} ;

supers: %empty | IDENT { if (strcmp($1->s, "extends")) word($1, "implements"); } exprs ;

of: IDENT { word($1, "of"); } ;
from: IDENT { word($1, "from"); } ;
extends: IDENT { word($1, "extends"); } ;

exprs: expr {} | exprs ',' expr {} ;

fndecl: FUNCTION IDENT { $<n>$ = open(N_FN, $2, @1); } tparams '(' params ')' opttype throws optbody {
    $$ = close($<n>3, $6, $8, $10);
    if (!$10 && !cheader) yyerror("expected a function body");
    if (!$10) $$->flags |= F_EXTERN; }
  ;

arrow: IDENT ARROW { Node *fn = open(N_FN, 0, @1); $<n>$ = push(fn, node(N_PARAM, @1, 0, 0, pid($1, @1))); } body {
    $$ = close($<n>3, 0, 0, $4); }
  | LPA { $<n>$ = open(N_FN, 0, @1); } params ')' ARROW body { $$ = close($<n>2, $3, 0, $6); }
  ;

body: LBLOCK { hoist(@1 + 1); } stmts '}' { $$ = $3; } | expr ;

classdecl: CLASS IDENT { $<n>$ = open(N_CLASS, $2, @1); } tparams supers tags '{' props '}' {
    $$ = close($<n>3, $8, 0, 0); $$->d = $6; }
  ;

/// Tags (spec 5.3) are types for reflection, kept in a class's or field's d.
tags: %empty { $$ = 0; } | tags '@' expr { $$ = push($1 ? $1 : mk(N_ARRAY, @2), $3); } ;

props: %empty { $$ = mk(N_OBJECT, @$); }
  | prop { $$ = push(mk(N_OBJECT, @1), $1); }
  | props sep
  | props sep prop { $$ = push($1, $3); }
  ;

sep: ',' | ';' ;

prop: ELLIPSIS expr { $$ = prop(F_SPREAD, 0, @1, $2, 0); }
  | mods key { $$ = prop($1, $2, @2, ident($2, @2), 0); }
  | mods key ':' expr tags { $$ = prop($1, $2, @2, $4, 0); $$->d = $5; }
  | mods key '=' expr { $$ = prop($1, $2, @2, 0, $4); }
  | mods key ':' expr tags '=' expr { $$ = prop($1, $2, @2, $4, $7); $$->d = $5; }
  | mods key ':' ELLIPSIS expr tags { $$ = prop($1 | F_EMBED, $2, @2, $5, 0); $$->d = $6; }
  | mods key ':' ELLIPSIS expr tags '=' expr { $$ = prop($1 | F_EMBED, $2, @2, $5, $8); $$->d = $6; }
  | mods key { $<n>$ = open(N_FN, 0, @2); $<n>$->flags |= F_METHOD; } tparams '(' params ')' opttype throws tags optbody {
      $$ = prop($1, $2, @2, 0, 0); $$->c = close($<n>3, $6, $8, $11); }
  ;

mods: %empty { $$ = 0; }
  | mods IDENT { $$ = $1 | modifier($2); }
  ;

optbody: %empty { $$ = 0; } | block ;

key: IDENT | kw | STR { $$ = $1->sym; } | INT { $$ = $1->sym; } ;

kw: AS | ASSERT | ASYNC | AWAIT | BREAK | CASE | CATCH | CLASS | CONST | CONTINUE | DEFAULT | ELSE | EXPORT
  | FALSE | FINALLY | FOR | FUNCTION | IF | IMPORT | INSTANCEOF | INTERFACE
  | LET | NEW | NUL | RETURN | SPAWN | SWITCH | TEST | THIS | THROW
  | TRUE | TRY | TYPE | TYPEOF | WHILE
  ;

ifstmt: IF '(' expr ')' block { $$ = node(N_IF, @1, $3, $5, 0); }
  | IF '(' expr ')' block ELSE block { $$ = node(N_IF, @1, $3, $5, $7); }
  | IF '(' expr ')' block ELSE ifstmt { $$ = node(N_IF, @1, $3, $5, $7); }
  ;

trystmt: TRY block catch { $$ = node(N_TRY, @1, $2, $3, 0); }
  | TRY block catch FINALLY block { $$ = node(N_TRY, @1, $2, $3, $5); }
  | TRY block FINALLY block { $$ = node(N_TRY, @1, $2, 0, $4); }
  ;

catch: CATCH block { $$ = node(N_CATCH, @1, 0, $2, 0); }
  | CATCH '(' mark pattern opttype ')' block { nbinds = $3; $$ = node(N_CATCH, @1, $5, $7, $4); }
  ;

scases: %empty { $$ = mk(N_SWITCH, @$); } | scases scase { $$ = push($1, $2); } ;

scase: CASE labels ':' stmts { $$ = $2; $$->a = $4; }
  | DEFAULT ':' stmts { $$ = node(N_CASE, @1, $3, 0, 0); }
  ;

ecases: %empty { $$ = mk(N_SWITCHX, @$); } | ecases ecase { $$ = push($1, $2); } | ecases ';' ;

ecase: CASE labels ':' expr { $$ = $2; $$->a = $4; }
  | DEFAULT ':' expr { $$ = node(N_CASE, @1, $3, 0, 0); }
  ;

labels: label { $$ = push(mk(N_CASE, @1), $1); } | labels ',' label { $$ = push($1, $3); } ;

label: expr | expr ELLIPSIS expr { $$ = node(N_RANGE, @1, $1, $3, 0); } ;

expr: bexpr | arrow
  | IF '(' expr ')' expr ELSE expr { $$ = node(N_IFX, @1, $3, $5, $7); }
  ;

bexpr: unary
  | bexpr QQ bexpr { $$ = bin(QQ, $1, $3, @2); }
  | bexpr OROR bexpr { $$ = bin(OROR, $1, $3, @2); }
  | bexpr ANDAND bexpr { $$ = bin(ANDAND, $1, $3, @2); }
  | bexpr CMP bexpr { $$ = cmp($2, $1, $3, @2); }
  | bexpr INSTANCEOF bexpr { $$ = bin(INSTANCEOF, $1, $3, @2); }
  | bexpr '|' bexpr { $$ = bin('|', $1, $3, @2); }
  | bexpr '^' bexpr { $$ = bin('^', $1, $3, @2); }
  | bexpr '&' bexpr { $$ = bin('&', $1, $3, @2); }
  | bexpr SHL bexpr { $$ = bin(SHL, $1, $3, @2); }
  | bexpr SHR bexpr { $$ = bin(SHR, $1, $3, @2); }
  | bexpr '+' bexpr { $$ = bin('+', $1, $3, @2); }
  | bexpr '-' bexpr { $$ = bin('-', $1, $3, @2); }
  | bexpr '*' bexpr { $$ = bin('*', $1, $3, @2); }
  | bexpr '/' bexpr { $$ = bin('/', $1, $3, @2); }
  | bexpr '%' bexpr { $$ = bin('%', $1, $3, @2); }
  | bexpr POW bexpr { $$ = bin(POW, $1, $3, @2); }
  | bexpr AS postfix { $$ = node(N_AS, @2, $1, $3, 0); }
  ;

unary: postfix
  | '!' unary { $$ = node(N_UNARY, @1, $2, 0, 0); $$->op = '!'; }
  | '-' unary { $$ = node(N_UNARY, @1, $2, 0, 0); $$->op = '-'; }
  | '~' unary { $$ = node(N_UNARY, @1, $2, 0, 0); $$->op = '~'; }
  | TYPEOF unary { $$ = node(N_UNARY, @1, $2, 0, 0); $$->op = TYPEOF; }
  ;

postfix: primary | postfix suffix { $$ = attach($2, $1); } ;

spostfix: sprimary | spostfix suffix { $$ = attach($2, $1); } ;

suffix: '.' IDENT { $$ = mk(N_MEMBER, @1); $$->sym = $2; }
  | QDOT IDENT { $$ = mk(N_MEMBER, @1); $$->sym = $2; $$->flags = F_OPT; }
  | '[' elems ']' { $$ = $2; $$->k = N_INDEX; }
  | QDOT '[' elems ']' { $$ = $3; $$->k = N_INDEX; $$->flags |= F_OPT; }
  | '(' elems ')' { $$ = $2; $$->k = N_CALL; }
  | QDOT '(' elems ')' { $$ = $3; $$->k = N_CALL; $$->flags |= F_OPT; }
  | '!' { $$ = mk(N_NONNULL, @1); }
  ;

sprimary: IDENT { $$ = ident($1, @1); }
  | INT | FLOAT | STR | template
  | TRUE { $$ = lit(@1, (Val){K_BOOL, .i = 1}); }
  | FALSE { $$ = lit(@1, (Val){K_BOOL}); }
  | NUL { $$ = lit(@1, (Val){K_NULL}); }
  | THIS { $$ = mk(N_THIS, @1); }
  | '[' elems ']' { $$ = $2; $$->k = N_ARRAY; }
  | '(' expr ')' { $$ = $2; $$->flags |= F_PAREN; }
  | NEW newcallee '(' elems ')' { $$ = $4; $$->k = N_NEW; $$->a = $2; }
  ;

primary: sprimary
  | '{' props '}' { $$ = $2; }
  | SWITCH '(' expr ')' '{' ecases '}' { $$ = $6; $$->a = $3; }
  ;

newcallee: primary
  | newcallee '.' IDENT { $$ = attach(mk(N_MEMBER, @2), $1); $$->sym = $3; }
  | newcallee '[' elems ']' { $$ = attach($3, $1); $$->k = N_INDEX; }
  ;

template: TPL | tpl TEND { $$ = push($1, $2); } ;

tpl: THEAD expr { $$ = push(push(mk(N_TPL, @1), $1), $2); }
  | tpl TMID expr { $$ = push(push($1, $2), $3); }
  ;

elems: %empty { $$ = mk(N_BLOCK, @$); }
  | elem { $$ = push(mk(N_BLOCK, @1), $1); }
  | elems ','
  | elems ',' elem { $$ = push($1, $3); }
  ;

elem: expr | ELLIPSIS expr { $$ = node(N_SPREAD, @1, $2, 0, 0); $$->flags = F_SPREAD; } ;

%%

int yylex(void) {
  Tok *t = &toks[pos];
  yylloc = pos++;
  if (t->n) yylval.n = t->n;
  else if (t->s) yylval.s = t->s;
  else yylval.i = t->op;
  return t->t;
}

void yyerror(const char *msg) {
  Tok *t = &toks[pos ? pos - 1 : 0];
  fprintf(stderr, "%s:%d:%d: %s\n", curfile, t->line, t->col, msg);
  exit(1);
}

Node *parse(const char *file, const char *src, long len, Mod *m, int header) {
  curfile = file, curmod = m, cheader = header;
  lexall(src, len);
  pos = nbinds = nctx = 0;
  yyparse();
  return program;
}
