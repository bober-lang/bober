%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "parse.h"
#define YY_DECL static int scan(void)

Tok *toks;
int ntoks;
static int line, col, tl, tc, nl, braces, tmid, nt, tdepth[256], top;
static char *buf;
static int blen, bcap;
static Sym *tsym;
static Node *tnode;

#define YY_USER_ACTION \
  if (YY_START == INITIAL) tl = line, tc = col; \
  for (char *p = yytext; *p; p++) if (*p == '\n') line++, col = 1; else col++;

static void put(int c) {
  if (blen == bcap) buf = realloc(buf, bcap = 2 * bcap + 64);
  buf[blen++] = c;
}

static void putu(unsigned c) {
  if (c < 0x80) put(c);
  else if (c < 0x800) put(0xC0 | c >> 6), put(0x80 | (c & 63));
  else if (c < 0x10000) put(0xE0 | c >> 12), put(0x80 | (c >> 6 & 63)), put(0x80 | (c & 63));
  else put(0xF0 | c >> 18), put(0x80 | (c >> 12 & 63)), put(0x80 | (c >> 6 & 63)), put(0x80 | (c & 63));
}

static int lit(int t, Val v) {
  tnode = alloc(sizeof(Node));
  tnode->val = v;
  return t;
}

static int str(int t) {
  lit(t, (Val){K_STR, .s = mkstr(buf, blen)});
  tnode->sym = intern(buf, blen);
  return t;
}

static int num(int base, int skip) {
  uint64_t x = 0;
  for (char *p = yytext + skip; *p; p++)
    if (*p != '_') x = x * base + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
  lit(INT, (Val){K_INT, .i = x});
  tnode->sym = intern(yytext, yyleng);
  return INT;
}

static int flt(void) {
  char t[128];
  int n = 0;
  for (char *p = yytext; *p && n < 127; p++) if (*p != '_') t[n++] = *p;
  t[n] = 0;
  return lit(FLOAT, (Val){K_FLT, .f = strtod(t, 0)});
}

static int esc(int c) { return c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c == '0' ? 0 : c; }

static void bad(void) {
  fprintf(stderr, "%s:%d:%d: unexpected character\n", curfile, line, col);
  exit(1);
}
%}

%option noyywrap nounput noinput never-interactive
%x INSTR INSQ INTPL
D [0-9][0-9_]*

%%

[ \t\r]+                  ;
\n                        nl = 1;
"//".*                    ;
[A-Za-z_$][A-Za-z0-9_$]*  { tsym = intern(yytext, yyleng); return tsym->tok ? tsym->tok : IDENT; }
0[xX][0-9a-fA-F_]+        return num(16, 2);
0[oO][0-7_]+              return num(8, 2);
0[bB][01_]+               return num(2, 2);
{D}                       return num(10, 0);
{D}"."{D}([eE][+-]?[0-9]+)?|{D}[eE][+-]?[0-9]+  return flt();
\"                        { blen = 0; BEGIN(INSTR); }
'                         { blen = 0; BEGIN(INSQ); }
`                         { blen = 0; tmid = 0; BEGIN(INTPL); }
<INSTR>\"                   { BEGIN(INITIAL); return str(STR); }
<INSQ>'                    { BEGIN(INITIAL); return str(STR); }
<INTPL>`                    { BEGIN(INITIAL); return str(tmid ? TEND : TPL); }
<INTPL>"${"                 { if (nt == 256) bad(); tdepth[nt++] = braces; BEGIN(INITIAL); return str(tmid ? TMID : THEAD); }
<INSTR,INSQ,INTPL>\\x[0-9a-fA-F]{2}       put(strtol(yytext + 2, 0, 16));
<INSTR,INSQ,INTPL>\\u\{[0-9a-fA-F]{1,6}\} putu(strtol(yytext + 3, 0, 16));
<INSTR,INSQ,INTPL>\\[nrt0\\'"`$]          put(esc(yytext[1]));
<INSTR>[^"\\\n]+            |
<INSQ>[^'\\\n]+            |
<INTPL>[^`\\$]+             |
<INTPL>"$"                  for (int i = 0; i < yyleng; i++) put(yytext[i]);
<INSTR,INSQ,INTPL>.|\n         bad();
"{"                       { braces++; return '{'; }
"}"                       { if (nt && braces == tdepth[nt - 1]) { nt--; blen = 0; tmid = 1; BEGIN(INTPL); } else { braces--; return '}'; } }
"..."                     return ELLIPSIS;
"?."                      return QDOT;
"=>"                      return ARROW;
"=="|"!="|"<="|">="|"<"|">" {
  top = yyleng == 1 ? yytext[0] : yytext[0] == '=' ? EQ : yytext[0] == '!' ? NE : yytext[0] == '<' ? LE : GE;
  return CMP;
}
"&&"                      return ANDAND;
"||"                      return OROR;
"??"                      return QQ;
"**"                      return POW;
"<<"                      return SHL;
">>"                      return SHR;
"++"                      return INC;
"--"                      return DEC;
([-+*/%&|^]|"**"|"<<"|">>"|"??")"=" {
  top = yyleng == 2 ? yytext[0] : yytext[0] == '*' ? POW : yytext[0] == '<' ? SHL : yytext[0] == '>' ? SHR : QQ;
  return ASSIGNOP;
}
[-+*/%=!&|^~()\[\],.;:@]  return yytext[0];
.                         bad();

%%

static const int ender[] = {IDENT, INT, FLOAT, STR, TPL, TEND, ')', ']', '}', '!', INC, DEC, THIS, NUL, TRUE, FALSE, -1};
static const int joiner[] = {')', ']', '}', ',', '.', ELLIPSIS, QDOT, ARROW, '=', ASSIGNOP, QQ, OROR, ANDAND,
  CMP, INSTANCEOF, '|', '^', '&', SHL, SHR, '+', '-', '*', '/', '%', POW,
  ELSE, CATCH, FINALLY, 0, -1};

static int keyahead(void) {
  char c = yy_hold_char, *p = yytext + yyleng;
  if (c == '(' || c == '[') return 1;
  while (c == ' ' || c == '\t') c = *++p;
  return c == ':' || c == '=';
}

static int in(int t, const int *set) {
  for (; *set >= 0; set++) if (*set == t) return 1;
  return 0;
}

void kwinit(void) {
  static const struct { const char *s; int tok; } kw[] = {{"as", AS}, {"assert", ASSERT}, {"async", ASYNC},
    {"await", AWAIT}, {"break", BREAK}, {"case", CASE}, {"catch", CATCH}, {"class", CLASS}, {"const", CONST},
    {"continue", CONTINUE}, {"default", DEFAULT}, {"else", ELSE}, {"export", EXPORT},
    {"false", FALSE}, {"finally", FINALLY}, {"for", FOR}, {"function", FUNCTION},
    {"if", IF}, {"import", IMPORT}, {"instanceof", INSTANCEOF}, {"interface", INTERFACE},
    {"let", LET}, {"new", NEW}, {"null", NUL},
    {"return", RETURN}, {"spawn", SPAWN}, {"switch", SWITCH}, {"test", TEST}, {"this", THIS},
    {"throw", THROW}, {"true", TRUE}, {"try", TRY}, {"type", TYPE}, {"typeof", TYPEOF},
    {"while", WHILE}};
  for (int i = 0; i < (int)(sizeof kw / sizeof *kw); i++) intern(kw[i].s, strlen(kw[i].s))->tok = kw[i].tok;
}

static void emit(Tok t) {
  static int cap;
  if (ntoks == cap) toks = realloc(toks, (cap = 2 * cap + 1024) * sizeof(Tok));
  toks[ntoks++] = t;
}

void lexall(const char *src, long len) {
  line = col = 1, braces = nt = nl = ntoks = 0;
  YY_BUFFER_STATE b = yy_scan_bytes(src, len);
  for (int t = 1; t;) {
    tsym = 0, tnode = 0, top = 0;
    t = scan();
    if (tnode) tnode->line = tl, tnode->col = tc, tnode->file = curfile;
    int p = ntoks ? toks[ntoks - 1].t : -1;
    if (tsym && tsym->tok && (p == '.' || p == QDOT)) t = IDENT;
    if (t == AS && (nl || !in(p, ender)) && p != '*') t = IDENT;
    if (nl && (p == RETURN || p == BREAK || p == CONTINUE || (in(p, ender) && (!in(t, joiner) || (tsym && tsym->tok && keyahead())))))
      emit((Tok){';', tl, tc});
    emit((Tok){t, tl, tc, top, tsym, tnode});
    nl = 0;
  }
  yy_delete_buffer(b);
  int *open = malloc(ntoks * sizeof(int)), sp = 0;
  for (int i = 0; i < ntoks; i++) {
    if (toks[i].t == '(') open[sp++] = i;
    if (toks[i].t == ')' && sp && toks[i + 1].t == ARROW) toks[open[sp - 1]].t = LPA;
    if (toks[i].t == ')' && sp) sp--;
    if (toks[i].t == ARROW && toks[i + 1].t == '{') toks[i + 1].t = LBLOCK;
  }
  free(open);
}
