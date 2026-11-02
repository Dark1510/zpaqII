/* zpaqII - ZPAQL compiler (text -> bytecode).
 * Port of libzpaq::Compiler (Unlicense, Matt Mahoney). */
#include "zq_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Component types and opcodes used by the Compiler. */
enum {
  NONE = 0, CONS = 1, CM = 2, ICM = 3, MATCH = 4, AVG = 5, MIX2 = 6, MIX = 7,
  ISSE = 8, SSE = 9,
  JT = 39, JF = 47, JMP = 63, LJ = 255,
  POST = 256, PCOMP, END, IF, IFNOT, ELSE, ENDIF, DO,
  WHILE, UNTIL, FOREVER, IFL, IFNOTL, ELSEL, SEMICOLON
};

const int zq_compsize[256] = {0, 2, 3, 2, 3, 4, 6, 6, 3, 5};

const char *const zq_compname[256] = {"",    "const", "cm",  "icm", "match",
                                      "avg", "mix2",  "mix", "isse", "sse",
                                      0};

const char *const zq_opcodelist[272] = {
    "error", "a++", "a--", "a!", "a=0", "", "",
    "a=r", "b<>a", "b++", "b--", "b!", "b=0", "",
    "", "b=r", "c<>a", "c++", "c--", "c!", "c=0",
    "", "", "c=r", "d<>a", "d++", "d--", "d!",
    "d=0", "", "", "d=r", "*b<>a", "*b++", "*b--",
    "*b!", "*b=0", "", "", "jt", "*c<>a", "*c++",
    "*c--", "*c!", "*c=0", "", "", "jf", "*d<>a",
    "*d++", "*d--", "*d!", "*d=0", "", "", "r=a",
    "halt", "out", "", "hash", "hashd", "", "",
    "jmp", "a=a", "a=b", "a=c", "a=d", "a=*b", "a=*c",
    "a=*d", "a=", "b=a", "b=b", "b=c", "b=d", "b=*b",
    "b=*c", "b=*d", "b=", "c=a", "c=b", "c=c", "c=d",
    "c=*b", "c=*c", "c=*d", "c=", "d=a", "d=b", "d=c",
    "d=d", "d=*b", "d=*c", "d=*d", "d=", "*b=a", "*b=b",
    "*b=c", "*b=d", "*b=*b", "*b=*c", "*b=*d", "*b=", "*c=a",
    "*c=b", "*c=c", "*c=d", "*c=*b", "*c=*c", "*c=*d", "*c=",
    "*d=a", "*d=b", "*d=c", "*d=d", "*d=*b", "*d=*c", "*d=*d",
    "*d=", "", "", "", "", "", "",
    "", "", "a+=a", "a+=b", "a+=c", "a+=d", "a+=*b",
    "a+=*c", "a+=*d", "a+=", "a-=a", "a-=b", "a-=c", "a-=d",
    "a-=*b", "a-=*c", "a-=*d", "a-=", "a*=a", "a*=b", "a*=c",
    "a*=d", "a*=*b", "a*=*c", "a*=*d", "a*=", "a/=a", "a/=b",
    "a/=c", "a/=d", "a/=*b", "a/=*c", "a/=*d", "a/=", "a%=a",
    "a%=b", "a%=c", "a%=d", "a%=*b", "a%=*c", "a%=*d", "a%=",
    "a&=a", "a&=b", "a&=c", "a&=d", "a&=*b", "a&=*c", "a&=*d",
    "a&=", "a&~a", "a&~b", "a&~c", "a&~d", "a&~*b", "a&~*c",
    "a&~*d", "a&~", "a|=a", "a|=b", "a|=c", "a|=d", "a|=*b",
    "a|=*c", "a|=*d", "a|=", "a^=a", "a^=b", "a^=c", "a^=d",
    "a^=*b", "a^=*c", "a^=*d", "a^=", "a<<=a", "a<<=b", "a<<=c",
    "a<<=d", "a<<=*b", "a<<=*c", "a<<=*d", "a<<=", "a>>=a", "a>>=b",
    "a>>=c", "a>>=d", "a>>=*b", "a>>=*c", "a>>=*d", "a>>=", "a==a",
    "a==b", "a==c", "a==d", "a==*b", "a==*c", "a==*d", "a==",
    "a<a", "a<b", "a<c", "a<d", "a<*b", "a<*c", "a<*d",
    "a<", "a>a", "a>b", "a>c", "a>d", "a>*b", "a>*c",
    "a>*d", "a>", "", "", "", "", "",
    "", "", "", "", "", "", "",
    "", "", "", "lj", "post", "pcomp", "end",
    "if", "ifnot", "else", "endif", "do", "while", "until",
    "forever", "ifl", "ifnotl", "elsel", ";", 0,
};

typedef struct zq_comp_ctx {
  const char *in;
  int *args;
  zq_zpaql *hz;
  zq_zpaql *pz;
  zq_writer *out2;
  int line;
  int state;
  zq_u16 *ifs;
  size_t iftop, ifcap;
  zq_u16 *dos;
  size_t dotop, docap;
} zq_comp_ctx;

/* Advance in to start of next token. Tokens are delimited by white space.
 * Comments enclosed in ((nested) parenthesis) are skipped. */
static void zq_next(zq_comp_ctx *c) {
  for (; *c->in; ++c->in) {
    if (*c->in == '\n') ++c->line;
    if (*c->in == '(') c->state += 1 + (c->state < 0);
    else if (c->state > 0 && *c->in == ')') --c->state;
    else if (c->state < 0 && *c->in <= ' ') c->state = 0;
    else if (c->state == 0 && *c->in > ' ') {
      c->state = -1;
      break;
    }
  }
  if (!*c->in) zq_error("unexpected end of config");
}

static int zq_tolower(int ch) {
  return (ch >= 'A' && ch <= 'Z') ? ch + 'a' - 'A' : ch;
}

/* true if in==word up to whitespace or '(', case insensitive */
static int zq_match_token(const zq_comp_ctx *c, const char *word) {
  const char *a = c->in;
  for (; (*a > ' ' && *a != '(' && *word); ++a, ++word)
    if (zq_tolower((unsigned char)*a) != zq_tolower((unsigned char)*word))
      return 0;
  return !*word && (*a <= ' ' || *a == '(');
}

static void zq_syntax_error(zq_comp_ctx *c, const char *msg,
                            const char *expected) {
  char s[128];
  size_t i;
  int r;
  s[0] = 0;
  strcat(s, "Config line ");
  for (i = strlen(s), r = 1000000; r; r /= 10)
    if (c->line / r) s[i++] = (char)('0' + c->line / r % 10);
  s[i] = 0;
  strcat(s, " at ");
  for (i = strlen(s); i < 40 && *c->in > ' '; ++i) s[i] = *c->in++;
  s[i] = 0;
  strcat(s, ": ");
  strncat(s, msg, 40);
  if (expected) {
    strcat(s, ", expected: ");
    strncat(s, expected, 20);
  }
  zq_error(s);
}

static int zq_rtoken_list(zq_comp_ctx *c, const char *const list[]) {
  int i;
  zq_next(c);
  for (i = 0; list[i]; ++i)
    if (zq_match_token(c, list[i])) return i;
  zq_syntax_error(c, "unexpected", NULL);
  return -1;
}

static void zq_rtoken_str(zq_comp_ctx *c, const char *s) {
  zq_next(c);
  if (!zq_match_token(c, s)) zq_syntax_error(c, "expected", s);
}

/* Reads a number in [low..high].  For "$N+M" returns args[N-1]+M. */
static int zq_rtoken_num(zq_comp_ctx *c, int low, int high) {
  int r = 0;
  zq_next(c);
  if (c->in[0] == '$' && c->in[1] >= '1' && c->in[1] <= '9') {
    if (c->in[2] == '+') r = atoi(c->in + 3);
    if (c->args) r += c->args[c->in[1] - '1'];
  } else if (c->in[0] == '-' || (c->in[0] >= '0' && c->in[0] <= '9')) {
    r = atoi(c->in);
  } else {
    zq_syntax_error(c, "expected a number", NULL);
  }
  if (r < low) zq_syntax_error(c, "number too low", NULL);
  if (r > high) zq_syntax_error(c, "number too high", NULL);
  return r;
}

static void zq_push(zq_u16 **s, size_t *top, size_t cap, zq_u16 x) {
  if (*top >= cap) zq_error("IF or DO nested too deep");
  (*s)[(*top)++] = x;
}

static zq_u16 zq_pop(zq_u16 *s, size_t *top) {
  if (*top <= 0) zq_error("unmatched IF or DO");
  return s[--(*top)];
}

/* Compiles HCOMP or PCOMP.  Returns the final token (POST, PCOMP, END). */
static int zq_compile_comp(zq_comp_ctx *c, zq_zpaql *z) {
  int op = 0;
  const int comp_begin = z->hend;
  for (;;) {
    int operand = -1;
    int operand2 = -1;
    op = zq_rtoken_list(c, zq_opcodelist);
    if (op == POST || op == PCOMP || op == END) break;

    if (op == IF) {
      op = JF;
      operand = 0;
      zq_push(&c->ifs, &c->iftop, c->ifcap, (zq_u16)(z->hend + 1));
    } else if (op == IFNOT) {
      op = JT;
      operand = 0;
      zq_push(&c->ifs, &c->iftop, c->ifcap, (zq_u16)(z->hend + 1));
    } else if (op == IFL || op == IFNOTL) {
      if (op == IFL) ZQ_HDR(z, z->hend++) = (zq_u8)JT;
      if (op == IFNOTL) ZQ_HDR(z, z->hend++) = (zq_u8)JF;
      ZQ_HDR(z, z->hend++) = 3;
      op = LJ;
      operand = operand2 = 0;
      zq_push(&c->ifs, &c->iftop, c->ifcap, (zq_u16)(z->hend + 1));
    } else if (op == ELSE || op == ELSEL) {
      int a;
      if (op == ELSE) {
        op = JMP;
        operand = 0;
      }
      if (op == ELSEL) {
        op = LJ;
        operand = operand2 = 0;
      }
      a = zq_pop(c->ifs, &c->iftop);
      if (ZQ_HDR(z, a - 1) != LJ) { /* IF, IFNOT */
        int j = z->hend - a + 1 + (op == LJ);
        if (j > 127) zq_syntax_error(c, "IF too big, try IFL, IFNOTL", NULL);
        ZQ_HDR(z, a) = (zq_u8)j;
      } else { /* IFL, IFNOTL */
        int j = z->hend - comp_begin + 2 + (op == LJ);
        ZQ_HDR(z, a) = (zq_u8)(j & 255);
        ZQ_HDR(z, a + 1) = (zq_u8)((j >> 8) & 255);
      }
      zq_push(&c->ifs, &c->iftop, c->ifcap, (zq_u16)(z->hend + 1));
    } else if (op == ENDIF) {
      int a = zq_pop(c->ifs, &c->iftop);
      int j = z->hend - a - 1;
      if (ZQ_HDR(z, a - 1) != LJ) {
        if (j > 127)
          zq_syntax_error(c, "IF too big, try IFL, IFNOTL, ELSEL\n", NULL);
        ZQ_HDR(z, a) = (zq_u8)j;
      } else {
        j = z->hend - comp_begin;
        ZQ_HDR(z, a) = (zq_u8)(j & 255);
        ZQ_HDR(z, a + 1) = (zq_u8)((j >> 8) & 255);
      }
    } else if (op == DO) {
      zq_push(&c->dos, &c->dotop, c->docap, (zq_u16)z->hend);
    } else if (op == WHILE || op == UNTIL || op == FOREVER) {
      int a = zq_pop(c->dos, &c->dotop);
      int j = a - z->hend - 2;
      if (j >= -127) { /* backward short jump */
        if (op == WHILE) op = JT;
        if (op == UNTIL) op = JF;
        if (op == FOREVER) op = JMP;
        operand = j & 255;
      } else { /* backward long jump */
        j = a - comp_begin;
        if (op == WHILE) {
          ZQ_HDR(z, z->hend++) = (zq_u8)JF;
          ZQ_HDR(z, z->hend++) = 3;
        }
        if (op == UNTIL) {
          ZQ_HDR(z, z->hend++) = (zq_u8)JT;
          ZQ_HDR(z, z->hend++) = 3;
        }
        op = LJ;
        operand = j & 255;
        operand2 = j >> 8;
      }
    } else if ((op & 7) == 7) { /* two-byte operand */
      if (op == LJ) {
        operand = zq_rtoken_num(c, 0, 65535);
        operand2 = operand >> 8;
        operand &= 255;
      } else if (op == JT || op == JF || op == JMP) {
        operand = zq_rtoken_num(c, -128, 127);
        operand &= 255;
      } else {
        operand = zq_rtoken_num(c, 0, 255);
      }
    }
    if (op >= 0 && op <= 255) ZQ_HDR(z, z->hend++) = (zq_u8)op;
    if (operand >= 0) ZQ_HDR(z, z->hend++) = (zq_u8)operand;
    if (operand2 >= 0) ZQ_HDR(z, z->hend++) = (zq_u8)operand2;
    if (z->hend >= (int)z->header.n - 130 ||
        z->hend - z->hbegin + z->cend - 2 > 65535)
      zq_syntax_error(c, "program too big", NULL);
  }
  ZQ_HDR(z, z->hend++) = 0; /* END */
  return op;
}

void zq_compile(const char *in, int *args, zq_zpaql *hz, zq_zpaql *pz,
                zq_writer *out2) {
  zq_comp_ctx c;
  int i, n, op;
  int hsize;
  memset(&c, 0, sizeof(c));
  c.in = in;
  c.args = args;
  c.hz = hz;
  c.pz = pz;
  c.out2 = out2;
  c.line = 1;
  c.state = 0;
  c.ifcap = c.docap = 1000;
  c.ifs = (zq_u16 *)zq_calloc(c.ifcap * sizeof(zq_u16));
  c.dos = (zq_u16 *)zq_calloc(c.docap * sizeof(zq_u16));

  zq_zpaql_clear(hz);
  zq_zpaql_clear(pz);
  zq_arr_resize(&hz->header, 68000, 1);

  /* COMP */
  zq_rtoken_str(&c, "comp");
  ZQ_HDR(hz, 2) = (zq_u8)zq_rtoken_num(&c, 0, 255);
  ZQ_HDR(hz, 3) = (zq_u8)zq_rtoken_num(&c, 0, 255);
  ZQ_HDR(hz, 4) = (zq_u8)zq_rtoken_num(&c, 0, 255);
  ZQ_HDR(hz, 5) = (zq_u8)zq_rtoken_num(&c, 0, 255);
  n = zq_rtoken_num(&c, 0, 255);
  ZQ_HDR(hz, 6) = (zq_u8)n;
  hz->cend = 7;
  for (i = 0; i < n; ++i) {
    int type, clen, j;
    zq_rtoken_num(&c, i, i);
    type = zq_rtoken_list(&c, zq_compname);
    ZQ_HDR(hz, hz->cend++) = (zq_u8)type;
    clen = zq_compsize[type & 255];
    if (clen < 1 || clen > 10) zq_syntax_error(&c, "invalid component", NULL);
    for (j = 1; j < clen; ++j)
      ZQ_HDR(hz, hz->cend++) = (zq_u8)zq_rtoken_num(&c, 0, 255);
  }
  ++hz->cend; /* COMP END (byte already 0) */
  hz->hbegin = hz->hend = hz->cend + 128;

  /* HCOMP */
  zq_rtoken_str(&c, "hcomp");
  op = zq_compile_comp(&c, hz);

  hsize = hz->cend - 2 + hz->hend - hz->hbegin;
  ZQ_HDR(hz, 0) = (zq_u8)(hsize & 255);
  ZQ_HDR(hz, 1) = (zq_u8)(hsize >> 8);

  if (op == POST) {
    zq_rtoken_num(&c, 0, 0);
    zq_rtoken_str(&c, "end");
  } else if (op == PCOMP) {
    int len;
    zq_arr_resize(&pz->header, 68000, 1);
    ZQ_HDR(pz, 4) = ZQ_HDR(hz, 4);
    ZQ_HDR(pz, 5) = ZQ_HDR(hz, 5);
    pz->cend = 8;
    pz->hbegin = pz->hend = pz->cend + 128;

    /* pcomp_cmd ends with ';' (case sensitive) */
    zq_next(&c);
    while (*c.in && *c.in != ';') {
      if (c.out2) c.out2->put(c.out2->self, (zq_u8)*c.in);
      ++c.in;
    }
    if (*c.in) ++c.in;

    op = zq_compile_comp(&c, pz);
    len = pz->cend - 2 + pz->hend - pz->hbegin;
    ZQ_HDR(pz, 0) = (zq_u8)(len & 255);
    ZQ_HDR(pz, 1) = (zq_u8)(len >> 8);
    if (op != END) zq_syntax_error(&c, "expected END", NULL);
  } else if (op != END) {
    zq_syntax_error(&c, "expected END or POST 0 END or PCOMP cmd ; ... END",
                    NULL);
  }

  free(c.ifs);
  free(c.dos);
}
