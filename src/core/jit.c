/* zpaqII - ZPAQL JIT (x86-64).
 *
 * Direct port of libzpaq 7.15 ZPAQL::assemble() (Unlicense, Matt Mahoney).
 * The generated code keeps the ZPAQL state in registers exactly like upstream:
 *
 *   edx = a    esi = b    edi = c    ebp = d    ebx = f
 *   r12 = h    r13 = outbuf    r14 = r    r15 = m
 *
 * and returns 0 on HALT, 1 for a bad opcode/jump, 2 for out of memory and
 * 3 for a write error.
 *
 * The x86-64 encoding below is upstream's, byte for byte; only the small
 * differences of this port show up: zq_arr instead of Array<>, and errors are
 * reported by longjmp from inside zq_zpaql_flush instead of exceptions, so
 * flush1 never returns nonzero.
 *
 * Only x86-64 is supported here (this port targets Linux x86-64).
 */
#include "zq_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef ZQ_NOJIT
#include <sys/mman.h>

/* Allocate/free executable memory (libzpaq::allocx). */
void zq_allocx(zq_u8 **p, int *n, int newsize) {
  if (*p || *n) {
    if (*p) munmap(*p, (size_t)*n);
    *p = NULL;
    *n = 0;
  }
  if (newsize > 0) {
    void *m = mmap(NULL, (size_t)newsize, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) {
      *p = NULL;
      *n = 0;
      zq_error("allocx failed");
    } else {
      *p = (zq_u8 *)m;
      *n = newsize;
    }
  }
}

/* int flush1(ZPAQL* z): called by the generated code when outbuf is full. */
static int zq_jit_flush(zq_zpaql *z) {
  zq_zpaql_flush(z);
  return 0;
}

/* Return true if op is an undefined ZPAQL instruction. */
static int jit_iserr(int op) {
  return op == 0 || (op >= 120 && op <= 127) || (op >= 240 && op <= 254) ||
         op == 58 || (op < 64 && (op % 8 == 5 || op % 8 == 6));
}

/* Length of the ZPAQL instruction at hcomp[0].  A run of identical ++ or --
 * is counted as one instruction (same as upstream oplen()). */
static int jit_oplen(const zq_u8 *hcomp) {
  if (*hcomp == 255) return 3;
  if (*hcomp % 8 == 7) return 2;
  if (*hcomp < 51 && (*hcomp % 8 - 1) / 2 == 0) {
    int i;
    for (i = 1; i < 127 && hcomp[i] == hcomp[0]; ++i) {
    }
    return i;
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Emitter buffer                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
  zq_u8 *code;  /* may be NULL while measuring the needed size */
  int cap;
} jit_buf;

/* Write k bytes of x MSB first (skipped past cap, still counted). */
static void jput(jit_buf *b, int *o, zq_u32 x, int k) {
  while (k-- > 0) {
    if (*o < b->cap) b->code[*o] = (zq_u8)((x >> (k * 8)) & 255);
    ++*o;
  }
}

/* Write 4 bytes of x LSB first. */
static void jput4lsb(jit_buf *b, int *o, zq_u32 x) {
  int k;
  for (k = 0; k < 4; ++k) {
    if (*o < b->cap) b->code[*o] = (zq_u8)((x >> (k * 8)) & 255);
    ++*o;
  }
}

/* Write an 8 byte pointer (x86-64) LSB first. */
static void jputptr(jit_buf *b, int *o, size_t p) {
  jput4lsb(b, o, (zq_u32)p);
  jput4lsb(b, o, (zq_u32)(p >> 32));
}

#define PUT1(x)      jput(&b, &o, (zq_u32)(x), 1)
#define PUT2(x)      jput(&b, &o, (zq_u32)(x), 2)
#define PUT3(x)      jput(&b, &o, (zq_u32)(x), 3)
#define PUT4(x)      jput(&b, &o, (zq_u32)(x), 4)
#define PUT5(x, y)   (PUT4(x), PUT1(y))
#define PUTA(x)      jput4lsb(&b, &o, (zq_u32)(uintptr_t)(x))
#define PUT1A(x, y)  (PUT1(x), PUTA(y))
#define PUT2A(x, y)  (PUT2(x), PUTA(y))
#define PUT3A(x, y)  (PUT3(x), PUTA(y))
#define PUT4A(x, y)  (PUT4(x), PUTA(y))
#define PUT5A(x, y, z) (PUT4(x), PUT1(y), PUTA(z))
#define PUT2L(x, p)  (PUT2(x), jputptr(&b, &o, (size_t)(uintptr_t)(p)))

/* Assemble the HCOMP section of z->header into z->rcode.  Never writes past
 * z->rcode_size; returns the number of bytes needed (0 on internal error). */
int zq_zpaql_assemble(zq_zpaql *z) {
  static const int regcode[8] = {2, 6, 7, 5, 0, 0, 0, 0};  /* a,b,c,d -> rdx,rsi,rdi,rbp */
  const zq_u8 *hcomp = (const zq_u8 *)z->header.data + z->hbegin;
  /* The interpreter rejects pc >= hend and header[hend-1] is the terminating
   * 0 (error opcode), so this is the exact code range.  Upstream adds 2 extra
   * slots here; they can only be reached by corrupt jumps, where this range
   * reproduces the interpreter's error instead of reading padding. */
  const int hlen = z->hend - z->hbegin;
  const int msize = (int)z->m.n;
  const int hsize = (int)z->h.n;
  jit_buf b;
  int *it;
  int done = 0;
  int o = 5;  /* reserve space for the jump to start */
  int halt, outlabel, start, rsize;
  int i, j, l;

  b.code = z->rcode;
  b.cap = z->rcode_size;

  if (hlen <= 0) return 0;
  it = (int *)calloc((size_t)hlen + 8, sizeof(int));
  if (!it) return 0;

  /* Code for HALT: store the registers back and return. */
  halt = o;
  PUT2L(0x48b9, &z->a);        /* mov rcx, a */
  PUT2(0x8911);                /* mov [rcx], edx */
  PUT2L(0x48b9, &z->b);        /* mov rcx, b */
  PUT2(0x8931);                /* mov [rcx], esi */
  PUT2L(0x48b9, &z->c);        /* mov rcx, c */
  PUT2(0x8939);                /* mov [rcx], edi */
  PUT2L(0x48b9, &z->d);        /* mov rcx, d */
  PUT2(0x8929);                /* mov [rcx], ebp */
  PUT2L(0x48b9, &z->f);        /* mov rcx, f */
  PUT2(0x8919);                /* mov [rcx], ebx */
  PUT4(0x4883c408);            /* add rsp, 8 */
  PUT2(0x415f);                /* pop r15 */
  PUT2(0x415e);                /* pop r14 */
  PUT2(0x415d);                /* pop r13 */
  PUT2(0x415c);                /* pop r12 */
  PUT1(0x5b);                  /* pop rbx */
  PUT1(0x5f);                  /* pop rdi */
  PUT1(0x5e);                  /* pop rsi */
  PUT1(0x5d);                  /* pop rbp */
  PUT1(0xc3);                  /* ret */

  /* Code for OUT: store a=edx at outbuf[bufptr++], flush when full. */
  outlabel = o;
  PUT2L(0x48b8, z->outbuf.data);        /* mov rax, outbuf */
  PUT2L(0x49ba, &z->bufptr);            /* mov r10, &bufptr */
  PUT3(0x418b0a);                       /* mov ecx, [r10] */
  PUT3(0x881408);                       /* mov [rax+rcx], dl */
  PUT2(0xffc1);                         /* inc rcx */
  PUT3(0x41890a);                       /* mov [r10], ecx */
  PUT2A(0x81f9, (int)z->outbuf.n);      /* cmp ecx, outbuf size */
  PUT2(0x7403);                         /* jz L1 */
  PUT2(0x31c0);                         /* xor eax, eax */
  PUT1(0xc3);                           /* ret */
  PUT1(0x55);                           /* L1: push rbp */
  PUT1(0x57);                           /* push rdi */
  PUT1(0x56);                           /* push rsi */
  PUT1(0x52);                           /* push rdx */
  PUT1(0x51);                           /* push rcx */
  PUT2L(0x48bf, z);                     /* mov rdi, this */
  PUT2L(0x49bb, &zq_jit_flush);         /* mov r11, &flush */
  PUT3(0x41ffd3);                       /* call r11 */
  PUT1(0x59);                           /* pop rcx */
  PUT1(0x5a);                           /* pop rdx */
  PUT1(0x5e);                           /* pop rsi */
  PUT1(0x5f);                           /* pop rdi */
  PUT1(0x5d);                           /* pop rbp */
  PUT1(0xc3);                           /* ret */

  /* it[i]=1 if reachable from the previous instruction, +2 if it is a jump
   * target (3 if both). */
  it[0] = 2;
  do {
    const int NONE = 0x80000000;
    done = 0;
    for (i = 0; i < hlen; ++i) {
      int op = hcomp[i];
      if (it[i]) {
        int next1 = i + jit_oplen(hcomp + i);
        int next2 = NONE;
        if (jit_iserr(op)) next1 = NONE;
        if (op == 56) {
          next1 = NONE;
          next2 = 0;
        }
        if (op == 255) {
          next1 = NONE;
          next2 = hcomp[i + 1] + 256 * hcomp[i + 2];
        }
        if (op == 39 || op == 47 || op == 63)
          next2 = i + 2 + (signed char)hcomp[i + 1];
        if (op == 63) next1 = NONE;
        if ((next2 < 0 || next2 >= hlen) && next2 != NONE) next2 = hlen - 1;
        if (next1 >= 0 && next1 < hlen && !(it[next1] & 1)) {
          it[next1] |= 1;
          ++done;
        }
        if (next2 >= 0 && next2 < hlen && !(it[next2] & 2)) {
          it[next2] |= 2;
          ++done;
        }
      }
    }
  } while (done > 0);

  /* Flag elision: bits 2-3 of it[i] become 4, 8 or 12 when a comparison does
   * not need to store f, or a jt/jf does not need to read it. */
  for (i = 0; i < hlen; ++i) {
    const int op1 = hcomp[i];
    const int i2 = i + 1 + (op1 % 8 == 7);
    const int op2 = hcomp[i2];
    if (it[i] && op1 >= 216 && op1 < 240 && (op2 == 39 || op2 == 47) &&
        it[i2] == 1 && (i2 == i + 1 || hcomp[i + 1] == 0)) {
      int code = (op1 - 208) / 8 * 4;
      it[i2] += code;
      for (j = 0; j < 2 && code; ++j) {
        int k = i2 + 2;
        if (j == 1) k = i2 + 2 + (signed char)hcomp[i2 + 1];
        for (l = 0; l < hlen && code; ++l) {
          int op;
          if (k < 0 || k >= hlen) break;
          op = hcomp[k];
          if (op == 39 || op == 47) code = 0;
          else if (op >= 216 && op < 240) break;
          else if (jit_iserr(op)) break;
          else if (op == 255) k = hcomp[k + 1] + 256 * hcomp[k + 2];
          else if (op == 63) k = k + 2 + (signed char)hcomp[k + 1];
          else if (op == 56) k = 0;
          else k = k + 1 + (op % 8 == 7);
        }
      }
      it[i] += code;
    }
  }

  /* Start of the generated function: save x86 registers, load ZPAQL state. */
  start = o;
  PUT1(0x55);                  /* push rbp */
  PUT1(0x56);                  /* push rsi */
  PUT1(0x57);                  /* push rdi */
  PUT1(0x53);                  /* push rbx */
  PUT2(0x4154);                /* push r12 */
  PUT2(0x4155);                /* push r13 */
  PUT2(0x4156);                /* push r14 */
  PUT2(0x4157);                /* push r15 */
  PUT4(0x4883ec08);            /* sub rsp, 8 */
  PUT2L(0x48b8, &z->a);        /* mov rax, a */
  PUT2(0x8b10);                /* mov edx, [rax] */
  PUT2L(0x48b8, &z->b);        /* mov rax, b */
  PUT2(0x8b30);                /* mov esi, [rax] */
  PUT2L(0x48b8, &z->c);        /* mov rax, c */
  PUT2(0x8b38);                /* mov edi, [rax] */
  PUT2L(0x48b8, &z->d);        /* mov rax, d */
  PUT2(0x8b28);                /* mov ebp, [rax] */
  PUT2L(0x48b8, &z->f);        /* mov rax, f */
  PUT2(0x8b18);                /* mov ebx, [rax] */
  PUT2L(0x49bc, z->h.data);    /* mov r12, h */
  PUT2L(0x49bd, z->outbuf.data);/* mov r13, outbuf */
  PUT2L(0x49be, z->r.data);    /* mov r14, r */
  PUT2L(0x49bf, z->m.data);    /* mov r15, m */

  /* Emit instructions in passes until every reachable byte is translated. */
  for (i = 0; i < hlen; ++i) {
    int istart = i;
    int inc = 0;
    int ii;
    for (ii = istart; ii < hlen && it[ii]; ii += inc) {
      const int code = it[ii];
      int op, arg, ddd, sss;
      inc = jit_oplen(hcomp + ii);

      /* Already assembled here? then jump to it and go on. */
      if (code >= 16) {
        if (ii > istart) {
          int rel = code - o;
          if (rel > -120 && rel < 120)
            PUT2(0xeb00 + ((rel - 2) & 255));  /* jmp short */
          else
            PUT1A(0xe9, rel - 5);              /* jmp near */
        }
        break;
      }

      it[ii] = o;
      op = hcomp[ii];
      arg = hcomp[ii + 1] + ((op == 255) ? 256 * hcomp[ii + 2] : 0);
      ddd = op / 8 % 8;
      sss = op % 8;

      /* Error instruction: return 1. */
      if (jit_iserr(op)) {
        PUT1A(0xb8, 1);            /* mov eax, 1 */
        PUT1A(0xe9, halt - o - 4); /* jmp near halt */
        continue;
      }

      /* Load the source *b, *c, *d or hash (*b) into eax, except:
       * {a,b,c,d}=*d and a{+,-,*,&,|,^,=,==,>,>}=*d load the address,
       * {a,b,c,d}={*b,*c} load the source into ddd. */
      if (op == 59 || (op >= 64 && op < 240 && op % 8 >= 4 && op % 8 < 7)) {
        int sz;
        int move;
        PUT2(0x89c0 + 8 * regcode[sss - 3 + (op == 59)]); /* mov eax,{esi,edi,ebp} */
        sz = (sss == 6 ? hsize : msize) - 1;
        if (sz >= 128) PUT1A(0x25, sz);        /* and eax, dword mask */
        else PUT3(0x83e000 + sz);              /* and eax, byte mask */
        move = (op >= 64 && op < 112);
        if (sss < 6) {
          if (move)
            PUT5(0x410fb604 + 8 * regcode[ddd], 0x07); /* movzx ddd,byte[r15+rax] */
          else
            PUT5(0x410fb604, 0x07);                    /* movzx eax,byte[r15+rax] */
        } else if ((0x06587000 >> (op / 8)) & 1) {
          PUT4(0x418b0484);      /* mov eax, [r12+rax*4] */
        }
      }

      /* Load the destination address *b, *c, *d or hashd (*d) into rcx. */
      if ((op >= 32 && op < 56 && op % 8 < 5) || (op >= 96 && op < 120) ||
          op == 60) {
        int sz;
        PUT2(0x89c1 + 8 * regcode[op / 8 % 8 - 3 - (op == 60)]); /* mov ecx,{esi,edi,ebp} */
        sz = (ddd == 6 || op == 60 ? hsize : msize) - 1;
        if (sz >= 128) PUT2A(0x81e1, sz);      /* and ecx, dword mask */
        else PUT3(0x83e100 + sz);              /* and ecx, byte mask */
        if (op / 8 % 8 == 6 || op == 60) {
          PUT4(0x498d0c8c);                    /* lea rcx,[r12+rcx*4] */
        } else {
          PUT4(0x498d0c0f);                    /* lea rcx,[r15+rcx] */
        }
      }

      switch ((op / 8) & 31) {
        case 0:   /* ddd = a */
        case 1:   /* ddd = b */
        case 2:   /* ddd = c */
        case 3:   /* ddd = d */
          switch (sss) {
            case 0:  /* ddd <> a */
              PUT2(0x87d0 + regcode[ddd]);     /* xchg edx, ddd */
              break;
            case 1:  /* ddd++ */
              PUT3(0x83c000 + 256 * regcode[ddd] + inc);
              break;
            case 2:  /* ddd-- */
              PUT3(0x83e800 + 256 * regcode[ddd] + inc);
              break;
            case 3:  /* ddd! */
              PUT2(0xf7d0 + regcode[ddd]);     /* not ddd */
              break;
            case 4:  /* ddd=0 */
              PUT2(0x31c0 + 9 * regcode[ddd]); /* xor ddd, ddd */
              break;
            case 7:  /* ddd=r n */
              PUT3A(0x418b86 + 8 * regcode[ddd], arg * 4);
              break;
          }
          break;
        case 4:   /* ddd = *b */
        case 5:   /* ddd = *c */
          switch (sss) {
            case 0:  /* ddd <> a */
              PUT2(0x8611);                    /* xchg dl, [rcx] */
              break;
            case 1:  /* ddd++ */
              PUT3(0x800100 + inc);            /* add byte [rcx], inc */
              break;
            case 2:  /* ddd-- */
              PUT3(0x802900 + inc);            /* sub byte [rcx], inc */
              break;
            case 3:  /* ddd! */
              PUT2(0xf611);                    /* not byte [rcx] */
              break;
            case 4:  /* ddd=0 */
              PUT2(0x31c0);                    /* xor eax, eax */
              PUT2(0x8801);                    /* mov [rcx], al */
              break;
            case 7: {  /* jt, jf */
              static const unsigned char jtab[2][4] = {{5, 4, 2, 7},
                                                       {4, 5, 3, 6}};
              if (code < 4) PUT2(0x84db);      /* test bl, bl */
              if (arg >= 128 && arg - 257 - ii >= 0 && o - it[arg - 257 - ii] < 120)
                PUT2(0x7000 + 256 * jtab[op == 47][code / 4]); /* jx short */
              else
                PUT2A(0x0f80 + jtab[op == 47][code / 4], 0);   /* jx near */
              break;
            }
          }
          break;
        case 6:   /* ddd = *d */
          switch (sss) {
            case 0:  /* ddd <> a */
              PUT2(0x8711);                    /* xchg edx, [rcx] */
              break;
            case 1:  /* ddd++ */
              PUT3(0x830100 + inc);            /* add dword [rcx], inc */
              break;
            case 2:  /* ddd-- */
              PUT3(0x832900 + inc);            /* sub dword [rcx], inc */
              break;
            case 3:  /* ddd! */
              PUT2(0xf711);                    /* not dword [rcx] */
              break;
            case 4:  /* ddd=0 */
              PUT2(0x31c0);                    /* xor eax, eax */
              PUT2(0x8901);                    /* mov [rcx], eax */
              break;
            case 7:  /* ddd=r n */
              PUT3A(0x418996, arg * 4);        /* mov [r14+n*4], edx */
              break;
          }
          break;
        case 7:   /* special */
          switch (op) {
            case 56:  /* halt */
              PUT2(0x31c0);                    /* xor eax, eax */
              PUT1A(0xe9, halt - o - 4);       /* jmp near halt */
              break;
            case 57:  /* out */
              PUT1A(0xe8, outlabel - o - 4);   /* call outlabel */
              PUT3(0x83f800);                  /* cmp eax, 0 */
              PUT2(0x7405);                    /* je L1 */
              PUT1A(0xe9, halt - o - 4);       /* jmp near halt */
              break;
            case 59:  /* hash: a = (a + *b + 512) * 773 */
              PUT3A(0x8d8410, 512);            /* lea edx, [eax+edx+512] */
              PUT2A(0x69d0, 773);              /* imul edx, eax, 773 */
              break;
            case 60:  /* hashd: *d = (*d + a + 512) * 773 */
              PUT2(0x8b01);                    /* mov eax, [rcx] */
              PUT3A(0x8d8410, 512);            /* lea eax, [eax+edx+512] */
              PUT2A(0x69c0, 773);              /* imul eax, eax, 773 */
              PUT2(0x8901);                    /* mov [rcx], eax */
              break;
            case 63:  /* jmp */
              PUT1A(0xe9, 0);                  /* fill in later */
              break;
          }
          break;
        case 8:   /* a= */
        case 9:   /* b= */
        case 10:  /* c= */
        case 11:  /* d= */
          if (sss == 7)
            PUT1A(0xb8 + regcode[ddd], arg);   /* mov ddd, n */
          else if (sss == 6) {
            PUT4(0x418b0484 + (regcode[ddd] << 11)); /* mov ddd,[r12+rax*4] */
          } else if (sss < 4) {
            PUT2(0x89c0 + regcode[ddd] + 8 * regcode[sss]); /* mov ddd, sss */
          }
          break;
        case 12:  /* *b= */
        case 13:  /* *c= */
          if (sss == 7) PUT3(0xc60100 + arg);  /* mov byte [rcx], n */
          else if (sss == 0) PUT2(0x8811);     /* mov byte [rcx], dl */
          else {
            if (sss < 4) PUT2(0x89c0 + 8 * regcode[sss]); /* mov eax, sss */
            PUT2(0x8801);                      /* mov byte [rcx], al */
          }
          break;
        case 14:  /* *d= */
          if (sss < 7) PUT2(0x8901 + 8 * regcode[sss]); /* mov [rcx], sss */
          else PUT2A(0xc701, arg);             /* mov dword [rcx], n */
          break;
        case 15:  /* not used */
          break;
        case 16:  /* a+= */
          if (sss == 6) {
            PUT4(0x41031484);                  /* add edx, [r12+rax*4] */
          } else if (sss < 7) PUT2(0x01c2 + 8 * regcode[sss]);
          else if (arg >= 128) PUT2A(0x81c2, arg);
          else PUT3(0x83c200 + arg);
          break;
        case 17:  /* a-= */
          if (sss == 6) {
            PUT4(0x412b1484);                  /* sub edx, [r12+rax*4] */
          } else if (sss < 7) PUT2(0x29c2 + 8 * regcode[sss]);
          else if (arg >= 128) PUT2A(0x81ea, arg);
          else PUT3(0x83ea00 + arg);
          break;
        case 18:  /* a*= */
          if (sss == 6) {
            PUT5(0x410faf14, 0x84);            /* imul edx, [r12+rax*4] */
          } else if (sss < 7) PUT3(0x0fafd0 + regcode[sss]);
          else if (arg >= 128) PUT2A(0x69d2, arg);
          else PUT3(0x6bd200 + arg);
          break;
        case 19:  /* a/= */
        case 20:  /* a%= */
          if (sss < 7) PUT2(0x89c1 + 8 * regcode[sss]); /* mov ecx, sss */
          else PUT1A(0xb9, arg);                       /* mov ecx, n */
          PUT2(0x85c9);                                /* test ecx, ecx */
          PUT3(0x0f44d1);                              /* cmovz edx, ecx */
          PUT2(0x7408 - 2 * (op / 8 == 20));           /* jz over */
          PUT2(0x89d0);                                /* mov eax, edx */
          PUT2(0x31d2);                                /* xor edx, edx */
          PUT2(0xf7f1);                                /* div ecx */
          if (op / 8 == 19) PUT2(0x89c2);              /* mov edx, eax */
          break;
        case 21:  /* a&= */
          if (sss == 6) {
            PUT4(0x41231484);                  /* and edx, [r12+rax*4] */
          } else if (sss < 7) PUT2(0x21c2 + 8 * regcode[sss]);
          else if (arg >= 128) PUT2A(0x81e2, arg);
          else PUT3(0x83e200 + arg);
          break;
        case 22:  /* a&=~ */
          if (sss == 7) {
            if (arg < 128) PUT3(0x83e200 + (~arg & 255));
            else PUT2A(0x81e2, ~arg);
          } else {
            if (sss < 4) PUT2(0x89c0 + 8 * regcode[sss]);
            PUT2(0xf7d0);                      /* not eax */
            PUT2(0x21c2);                      /* and edx, eax */
          }
          break;
        case 23:  /* a|= */
          if (sss == 6) {
            PUT4(0x410b1484);                  /* or edx, [r12+rax*4] */
          } else if (sss < 7) PUT2(0x09c2 + 8 * regcode[sss]);
          else if (arg >= 128) PUT2A(0x81ca, arg);
          else PUT3(0x83ca00 + arg);
          break;
        case 24:  /* a^= */
          if (sss == 6) {
            PUT4(0x41331484);                  /* xor edx, [r12+rax*4] */
          } else if (sss < 7) PUT2(0x31c2 + 8 * regcode[sss]);
          else if (arg >= 128) PUT2A(0x81f2, arg);
          else PUT3(0x83f200 + arg);
          break;
        case 25:  /* a<<= */
        case 26:  /* a>>= */
          if (sss == 7)
            PUT3(0xc1e200 + 8 * 256 * (op / 8 == 26) + arg);
          else {
            PUT2(0x89c1 + 8 * regcode[sss]);   /* mov ecx, sss */
            PUT2(0xd3e2 + 8 * (op / 8 == 26)); /* shl/shr edx, cl */
          }
          break;
        case 27:  /* a== */
        case 28:  /* a< */
        case 29:  /* a> */
          if (sss == 6) {
            PUT4(0x413b1484);                  /* cmp edx, [r12+rax*4] */
          } else if (sss == 7) PUT2A(0x81fa, arg);
          else PUT2(0x39c2 + 8 * regcode[sss]);
          if (code < 4) {
            if (op / 8 == 27) PUT3(0x0f94c3);  /* setz bl */
            if (op / 8 == 28) PUT3(0x0f92c3);  /* setc bl */
            if (op / 8 == 29) PUT3(0x0f97c3);  /* seta bl */
          }
          break;
        case 30:  /* not used */
        case 31:  /* 255 = lj */
          if (op == 255) PUT1A(0xe9, 0);       /* fill in later */
          break;
      }
    }
  }

  /* First pass done: if it did not fit, report the size needed. */
  rsize = o;
  if (o > b.cap) {
    free(it);
    return rsize;
  }

  /* Second pass: fill in the jump targets. */
  for (i = 0; i < hlen; ++i) {
    int target;
    int op;
    if (it[i] < 16) continue;
    op = hcomp[i];
    if (op != 39 && op != 47 && op != 63 && op != 255) continue;
    target = hcomp[i + 1];
    if (op == 255) {
      target += hcomp[i + 2] * 256;
    } else {
      if (target >= 128) target -= 256;
      target += i + 2;
    }
    if (target < 0 || target >= hlen) target = hlen - 1; /* runtime error */
    o = it[i];
    if ((op == 39 || op == 47) && b.code[o] == 0x84) o += 2;
    if (b.code[o] == 0x0f) ++o;
    op = b.code[o++];
    target = it[target] - o;
    if ((op >= 0x72 && op < 0x78) || op == 0xeb) {
      --target;
      if (target < -128 || target > 127) {
        /* Cannot happen for code assembled above; fall back to the
         * interpreter rather than run a corrupt jump. */
        free(it);
        return 0;
      }
      b.code[o] = (zq_u8)(target & 255);
    } else if ((op >= 0x82 && op < 0x88) || op == 0xe9) {
      target -= 4;
      jput4lsb(&b, &o, (zq_u32)target);
    } else {
      free(it);
      return 0;
    }
  }

  /* Jump to the start of the generated function. */
  o = 0;
  PUT1A(0xe9, start - 5);
  free(it);
  return rsize;
}

#else /* ZQ_NOJIT */

void zq_allocx(zq_u8 **p, int *n, int newsize) {
  (void)newsize;
  *p = NULL;
  *n = 0;
}

int zq_zpaql_assemble(zq_zpaql *z) {
  (void)z;
  return 0;
}

#endif
