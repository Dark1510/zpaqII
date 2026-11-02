/* zpaqII - predictor JIT (x86-64).
 *
 * Direct port of libzpaq 7.15 Predictor::assemble_p() (Unlicense, Matt
 * Mahoney).  The generated code is two functions stored in one buffer:
 *
 *   pcode[0..4]   jmp predict      (predict starts at pcode[10])
 *   pcode[5..9]   jmp update       (update  starts at pcode[15])
 *
 * predict() takes the Predictor address in rdi and returns squash(p[n-1]) in
 * eax.  update() takes the Predictor in rdi and the bit y in rsi (Linux-64
 * ABI) and leaves the c8/hmap4 bookkeeping and the HCOMP call to the C
 * wrapper, exactly like upstream.
 *
 * The x86-64 encoding below is upstream's, byte for byte; the offsets come
 * from our structures, whose member order and widths match libzpaq's
 * (Component: limit, cxt, a, b, c, then the three arrays, whose first member
 * is the data pointer), so the generated code addresses them the same way.
 *
 * Only x86-64 is supported here (this port targets Linux x86-64).
 */
#include "zq_internal.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef ZQ_NOJIT

/* ------------------------------------------------------------------ */
/* Emitter                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
  zq_u8 *code; /* may be NULL while measuring the needed size */
  int cap;
} pjit_buf;

/* Write k bytes of x MSB first (skipped past cap, still counted). */
static void pput(pjit_buf *b, int *o, zq_u32 x, int k) {
  while (k-- > 0) {
    if (*o < b->cap) b->code[*o] = (zq_u8)((x >> (k * 8)) & 255);
    ++*o;
  }
}

/* Write 4 bytes of x LSB first. */
static void pput4lsb(pjit_buf *b, int *o, zq_u32 x) {
  int k;
  for (k = 0; k < 4; ++k) {
    if (*o < b->cap) b->code[*o] = (zq_u8)((x >> (k * 8)) & 255);
    ++*o;
  }
}

#define PUT1(x)      pput(&b, &o, (zq_u32)(x), 1)
#define PUT2(x)      pput(&b, &o, (zq_u32)(x), 2)
#define PUT3(x)      pput(&b, &o, (zq_u32)(x), 3)
#define PUT4(x)      pput(&b, &o, (zq_u32)(x), 4)
#define PUT5(x, y)   (PUT4(x), PUT1(y))
#define PUTA(x)      pput4lsb(&b, &o, (zq_u32)(x))
#define PUT1A(x, y)  (PUT1(x), PUTA(y))
#define PUT2A(x, y)  (PUT2(x), PUTA(y))
#define PUT3A(x, y)  (PUT3(x), PUTA(y))
#define PUT4A(x, y)  (PUT4(x), PUTA(y))

/* Field offsets in the predictor and its components.  OFFC is the absolute
 * offset of comp[i].f, so it needs the loop index (same as upstream offc). */
#define OFF(f)   ((int)offsetof(zq_predictor, f))
#define OFFC(f)  ((int)(offsetof(zq_predictor, comp) + \
                  (i) * (int)sizeof(zq_component) + offsetof(zq_component, f)))
#define OFF_H(k) (OFF(h) + 4 * (int)(k))
#define OFF_P(k) (OFF(p) + 4 * (int)(k))

/* Assemble the COMP list of z->header into pr->pcode.  Never writes past
 * pr->pcode_size; returns the number of bytes needed. */
int zq_pred_assemble(zq_predictor *pr) {
  pjit_buf b;
  zq_u8 *hcomp = (zq_u8 *)pr->z->header.data;
  int o = 0;
  int i, n;
  zq_u8 *cp;
  int save_o;

  b.code = pr->pcode;
  b.cap = pr->pcode_size;

  /* Initialize for predict().  Put the predictor address in rdi. */
  PUT1A(0xe9, 5);          /* jmp predict */
  PUT1A(0, 0x90909000);    /* reserve space for jmp update */
  PUT1(0x53);              /* push rbx */
  PUT1(0x55);              /* push rbp */
  PUT1(0x56);              /* push rsi */
  PUT1(0x57);              /* push rdi */

  /* Code predict() for each component */
  n = hcomp[6];
  cp = hcomp + 7;
  for (i = 0; i < n; ++i, cp += zq_compsize[cp[0]]) {
    if (cp - hcomp >= pr->z->cend) zq_error("comp too big");
    if (cp[0] < 1 || cp[0] > 9) zq_error("invalid component");
    switch (cp[0]) {

      case ZQ_CONS:
        break;

      case ZQ_CM:
        /* cr.cxt=h[i]^hmap4; p[i]=stretch(cr.cm(cr.cxt)>>17); */
        PUT2A(0x8b87, OFF_H(i));            /* mov eax,[edi+&h[i]] */
        PUT2A(0x3387, OFF(hmap4));          /* xor eax,[edi+&hmap4] */
        PUT1A(0x25, (1 << cp[1]) - 1);      /* and eax,size-1 */
        PUT2A(0x8987, OFFC(cxt));           /* mov [edi+cxt],eax */
        PUT1(0x48);                         /* rex.w */
        PUT2A(0x8bb7, OFFC(cm));            /* mov rsi,[edi+&cm] */
        PUT3(0x8b0486);                     /* mov eax,[rsi+rax*4] */
        PUT3(0xc1e811);                     /* shr eax,17 */
        PUT4A(0x0fbf8447, OFF(stretcht));   /* movsx eax,word[edi+rax*2+..] */
        PUT2A(0x8987, OFF_P(i));            /* mov [edi+&p[i]],eax */
        break;

      case ZQ_ISSE:
      case ZQ_ICM:
        /* if (c8==1 || (c8&0xf0)==16) cr.c=find(cr.ht, cp[1]+2, h[i]+16*c8);
           cr.cxt=cr.ht[cr.c+(hmap4&15)];
           ICM:  p[i]=stretch(cr.cm(cr.cxt)>>8);
           ISSE: wt=(int*)&cr.cm[cr.cxt*2];
                 p[i]=clamp2k((wt[0]*p[cp[2]]+wt[1]*64)>>16); */

        PUT1(0x48);                         /* rex.w */
        PUT2A(0x8bb7, OFFC(ht));            /* mov rsi,[edi+&ht] */
        PUT2(0x8b07);                       /* mov eax,[edi] ; c8 */
        PUT2(0x89c1);                       /* mov ecx,eax */
        PUT3(0x83f801);                     /* cmp eax,1 */
        PUT2(0x740a);                       /* je L1 */
        PUT1A(0x25, 240);                   /* and eax,0xf0 */
        PUT3(0x83f810);                     /* cmp eax,16 */
        PUT2(0x7576);                       /* jne L2 */
        /* L1: find cxt in ht, return index in eax */
        PUT3(0xc1e104);                     /* shl ecx,4 */
        PUT2A(0x038f, OFF_H(i));            /* add [edi+&h[i]],ecx */
        PUT2(0x89c8);                       /* mov eax,ecx */
        PUT3(0xc1e902 + cp[1]);             /* shr ecx,sizebits+2 */
        PUT2A(0x81e1, 255);                 /* and eax,255 ; chk */
        PUT3(0xc1e004);                     /* shl eax,4 */
        PUT1A(0x25, (64 << cp[1]) - 16);    /* and eax,ht.size()-16 */
        PUT3(0x3a0c06);                     /* cmp cl,[rsi+rax] */
        PUT2(0x744d);                       /* je L3 */
        PUT3(0x83f010);                     /* xor eax,16 */
        PUT3(0x3a0c06);                     /* cmp cl,[rsi+rax] */
        PUT2(0x7445);                       /* je L3 */
        PUT3(0x83f030);                     /* xor eax,48 */
        PUT3(0x3a0c06);                     /* cmp cl,[rsi+rax] */
        PUT2(0x743d);                       /* je L3 */
        /* replace the row with the lowest priority */
        PUT3(0x83f021);                     /* xor eax,33 ; h0+1 */
        PUT3(0x8a1c06);                     /* mov bl,[rsi+rax] */
        PUT2(0x89c2);                       /* mov edx,eax */
        PUT3(0x83f220);                     /* xor edx,32 */
        PUT3(0x3a1c16);                     /* cmp bl,[rsi+rdx] */
        PUT2(0x7708);                       /* ja L4 */
        PUT3(0x83f230);                     /* xor edx,48 */
        PUT3(0x3a1c16);                     /* cmp bl,[rsi+rdx] */
        PUT2(0x7611);                       /* jbe L7 */
        /* L4: h0 is not lowest, replace h1 or h2 */
        PUT3(0x83f010);                     /* xor eax,16 */
        PUT3(0x8a1c06);                     /* mov bl,[rsi+rax] */
        PUT3(0x83f030);                     /* xor eax,48 */
        PUT3(0x3a1c06);                     /* cmp bl,[rsi+rax] */
        PUT2(0x7303);                       /* jae L7 */
        PUT3(0x83f030);                     /* xor eax,48 */
        /* L7: replace row pointed to by eax */
        PUT3(0x83f001);                     /* xor eax,1 */
        PUT3(0x890c06);                     /* mov [rsi+rax],ecx ; chk */
        PUT2(0x31c9);                       /* xor ecx,ecx */
        PUT4(0x894c0604);                   /* mov [rsi+rax+4],ecx */
        PUT4(0x894c0608);                   /* mov [rsi+rax+8],ecx */
        PUT4(0x894c060c);                   /* mov [rsi+rax+12],ecx */
        /* L3: save the nibble context in c */
        PUT2A(0x8987, OFFC(c));             /* mov [edi+c],eax */
        PUT2(0xeb06);                       /* jmp L8 */
        /* L2: get the nibble context */
        PUT2A(0x8b87, OFFC(c));             /* mov eax,[edi+c] */
        /* L8: nibble context in eax */
        PUT2A(0x8b97, OFF(hmap4));          /* mov edx,[edi+&hmap4] */
        PUT3(0x83e20f);                     /* and edx,15 */
        PUT2(0x01d0);                       /* add eax,edx */
        PUT4(0x0fb61406);                   /* movzx edx,byte[rsi+rax] */
        PUT2A(0x8997, OFFC(cxt));           /* mov [edi+cxt],edx */
        PUT1(0x48);                         /* rex.w */
        PUT2A(0x8bb7, OFFC(cm));            /* mov rsi,[edi+&cm] */
        if (cp[0] == ZQ_ICM) {
          PUT3(0x8b0496);                   /* mov eax,[rsi+rdx*4] */
          PUT3(0xc1e808);                   /* shr eax,8 */
          PUT4A(0x0fbf8447, OFF(stretcht)); /* movsx eax,word[edi+rax*2+..] */
        } else {
          PUT2A(0x8b87, OFF_P(cp[2]));      /* mov eax,[edi+&p[j]] */
          PUT4(0x0faf04d6);                 /* imul eax,[rsi+rdx*8] */
          PUT4(0x8b4cd604);                 /* mov ecx,[rsi+rdx*8+4] */
          PUT3(0xc1e106);                   /* shl ecx,6 */
          PUT2(0x01c8);                     /* add eax,ecx */
          PUT3(0xc1f810);                   /* sar eax,16 */
          PUT1A(0xb9, 2047);                /* mov ecx,2047 */
          PUT2(0x39c8);                     /* cmp eax,ecx */
          PUT3(0x0f4fc1);                   /* cmovg eax,ecx */
          PUT1A(0xb9, -2048);               /* mov ecx,-2048 */
          PUT2(0x39c8);                     /* cmp eax,ecx */
          PUT3(0x0f4cc1);                   /* cmovl eax,ecx */
        }
        PUT2A(0x8987, OFF_P(i));            /* mov [edi+&p[i]],eax */
        break;

      case ZQ_MATCH:
        /* if (cr.a==0) p[i]=0; else { c=(ht(limit-b)>>(7-cxt))&1;
             p[i]=stretch(dt2k[a]*(c*-2+1)&32767); } */
        PUT1(0x48);                         /* rex.w */
        PUT2A(0x8bb7, OFFC(ht));            /* mov rsi,[edi+&ht] */
        PUT2A(0x8b87, OFFC(a));             /* mov eax,[edi+&a] */
        PUT2(0x85c0);                       /* test eax,eax */
        PUT2(0x7449);                       /* jz L2 */
        PUT1A(0xb9, 7);                     /* mov ecx,7 */
        PUT2A(0x2b8f, OFFC(cxt));           /* sub ecx,[edi+&cxt] */
        PUT2A(0x8b87, OFFC(limit));         /* mov eax,[edi+&limit] */
        PUT2A(0x2b87, OFFC(b));             /* sub eax,[edi+&b] */
        PUT1A(0x25, (1 << cp[2]) - 1);      /* and eax,ht.size()-1 */
        PUT4(0x0fb60406);                   /* movzx eax,byte[rsi+rax] */
        PUT2(0xd3e8);                       /* shr eax,cl */
        PUT3(0x83e001);                     /* and eax,1 */
        PUT2A(0x8987, OFFC(c));             /* mov [edi+&c],eax */
        PUT2A(0x8b87, OFFC(a));             /* mov eax,[edi+&a] */
        PUT3A(0x8b8487, OFF(dt2k));         /* mov eax,[edi+rax*4+&dt2k] */
        PUT2(0x7402);                       /* jz L1 */
        PUT2(0xf7d8);                       /* neg eax */
        PUT1A(0x25, 0x7fff);                /* L1: and eax,32767 */
        PUT4A(0x0fbf8447, OFF(stretcht));   /* movsx eax,word[edi+rax*2+..] */
        PUT2A(0x8987, OFF_P(i));            /* L2: mov [edi+&p[i]],eax */
        break;

      case ZQ_AVG:
        /* p[i]=(p[j]*wt+p[k]*(256-wt))>>8; */
        PUT2A(0x8b87, OFF_P(cp[1]));        /* mov eax,[edi+&p[j]] */
        PUT2A(0x2b87, OFF_P(cp[2]));        /* sub eax,[edi+&p[k]] */
        PUT2A(0x69c0, cp[3]);               /* imul eax,wt */
        PUT3(0xc1f808);                     /* sar eax,8 */
        PUT2A(0x0387, OFF_P(cp[2]));        /* add eax,[edi+&p[k]] */
        PUT2A(0x8987, OFF_P(i));            /* mov [edi+&p[i]],eax */
        break;

      case ZQ_MIX2:
        /* cxt=((h[i]+(c8&mask))&(c-1)); w=a16[cxt];
           p[i]=(w*p[j]+(65536-w)*p[k])>>16; */
        PUT2(0x8b07);                       /* mov eax,[edi] ; c8 */
        PUT1A(0x25, cp[5]);                 /* and eax,mask */
        PUT2A(0x0387, OFF_H(i));            /* add eax,[edi+&h[i]] */
        PUT1A(0x25, (1 << cp[1]) - 1);      /* and eax,size-1 */
        PUT2A(0x8987, OFFC(cxt));           /* mov [edi+&cxt],eax */
        PUT1(0x48);                         /* rex.w */
        PUT2A(0x8bb7, OFFC(a16));           /* mov rsi,[edi+&a16] */
        PUT4(0x0fb70446);                   /* movzx eax,word[rsi+rax*2] */
        PUT2A(0x8b8f, OFF_P(cp[2]));        /* mov ecx,[edi+&p[j]] */
        PUT2A(0x8b97, OFF_P(cp[3]));        /* mov edx,[edi+&p[k]] */
        PUT2(0x29d1);                       /* sub ecx,edx */
        PUT3(0x0fafc8);                     /* imul ecx,eax */
        PUT3(0xc1e210);                     /* shl edx,16 */
        PUT2(0x01d1);                       /* add ecx,edx */
        PUT3(0xc1f910);                     /* sar ecx,16 */
        PUT2A(0x898f, OFF_P(i));            /* mov [edi+&p[i]],ecx */
        break;

      case ZQ_MIX: {
        /* cxt=(h[i]+(c8&mask))&(c-1); cxt*=m; wt=&cm[cxt];
           p[i]=clamp2k((sum (wt[j]>>8)*p[j])>>8); */
        int k;
        PUT2(0x8b07);                       /* mov eax,[edi] ; c8 */
        PUT1A(0x25, cp[5]);                 /* and eax,mask */
        PUT2A(0x0387, OFF_H(i));            /* add eax,[edi+&h[i]] */
        PUT1A(0x25, (1 << cp[1]) - 1);      /* and eax,size-1 */
        PUT2A(0x69c0, cp[3]);               /* imul eax,m */
        PUT2A(0x8987, OFFC(cxt));           /* mov [edi+&cxt],eax */
        PUT1(0x48);                         /* rex.w */
        PUT2A(0x8bb7, OFFC(cm));            /* mov rsi,[edi+&cm] */
        PUT1(0x48);                         /* rex.w */
        PUT3(0x8d3486);                     /* lea rsi,[rsi+rax*4] */
        /* Unroll the summation: xmm1/xmm2 = wt, xmm3/xmm4 = p */
        for (k = 0; k < cp[3]; k += 8) {
          const int tail = cp[3] - k;
          PUT4A(0xf30f6f8e, k * 4);         /* movdqu xmm1,[rsi+k*4] */
          if (tail > 3) PUT4A(0xf30f6f96, k * 4 + 16);
          PUT5(0x660f72e1, 0x08);           /* psrad xmm1,8 */
          if (tail > 3) PUT5(0x660f72e2, 0x08);
          PUT4(0x660f6bca);                 /* packssdw xmm1,xmm2 */
          PUT4A(0xf30f6f9f, OFF_P(cp[2] + k));
          if (tail > 3) PUT4A(0xf30f6fa7, OFF_P(cp[2] + k + 4));
          PUT4(0x660f6bdc);                 /* packssdw xmm3,xmm4 */
          if (tail > 0 && tail < 8) {
            PUT4(0x660f76ed);               /* pcmpeqd xmm5,xmm5 */
            PUT5(0x660f73dd, 16 - tail * 2); /* psrldq xmm5,16-tail*2 */
            PUT4(0x660fdbcd);               /* pand xmm1,xmm5 */
          }
          if (k == 0) {
            PUT4(0xf30f6fc1);               /* movdqu xmm0,xmm1 */
            PUT4(0x660ff5c3);               /* pmaddwd xmm0,xmm3 */
          } else {
            PUT4(0x660ff5cb);               /* pmaddwd xmm1,xmm3 */
            PUT4(0x660ffec1);               /* paddd xmm0,xmm1 */
          }
        }
        PUT4(0xf30f6fc8);                   /* movdqu xmm1,xmm0 */
        PUT5(0x660f73d9, 0x08);             /* psrldq xmm1,8 */
        PUT4(0x660ffec1);                   /* paddd xmm0,xmm1 */
        PUT4(0xf30f6fc8);                   /* movdqu xmm1,xmm0 */
        PUT5(0x660f73d9, 0x04);             /* psrldq xmm1,4 */
        PUT4(0x660ffec1);                   /* paddd xmm0,xmm1 */
        PUT4(0x660f7ec0);                   /* movd eax,xmm0 */
        PUT3(0xc1f808);                     /* sar eax,8 */
        PUT1A(0x3d, 2047);                  /* cmp eax,2047 */
        PUT2(0x7e05);                       /* jle L1 */
        PUT1A(0xb8, 2047);                  /* mov eax,2047 */
        PUT1A(0x3d, -2048);                 /* L1: cmp eax,-2048 */
        PUT2(0x7d05);                       /* jge L2 */
        PUT1A(0xb8, -2048);                 /* mov eax,-2048 */
        PUT2A(0x8987, OFF_P(i));            /* L2: mov [edi+&p[i]],eax */
        break;
      }

      case ZQ_SSE:
        /* cxt=(h[i]+c8)*32; pq=p[j]+992 clamped to 0..1983; wt=pq&63;
           pq>>=6; cxt+=pq; p[i]=stretch(...); cxt+=wt>>5; */
        PUT2A(0x8b8f, OFF_H(i));            /* mov ecx,[edi+&h[i]] */
        PUT2(0x030f);                       /* add ecx,[edi] ; c8 */
        PUT2A(0x81e1, (1 << cp[1]) - 1);    /* and ecx,size-1 */
        PUT3(0xc1e105);                     /* shl ecx,5 */
        PUT2A(0x8b87, OFF_P(cp[2]));        /* mov eax,[edi+&p[j]] */
        PUT1A(0x05, 992);                   /* add eax,992 */
        PUT2(0x31d2);                       /* xor edx,edx */
        PUT2(0x39d0);                       /* cmp eax,edx */
        PUT3(0x0f4cc2);                     /* cmovl eax,edx */
        PUT1A(0xba, 1983);                  /* mov edx,1983 */
        PUT2(0x39d0);                       /* cmp eax,edx */
        PUT3(0x0f4fc2);                     /* cmovg eax,edx */
        PUT2(0x89c2);                       /* mov edx,eax */
        PUT3(0x83e23f);                     /* and edx,63 ; wt */
        PUT3(0xc1e806);                     /* shr eax,6 ; pq */
        PUT2(0x01c1);                       /* add ecx,eax */
        PUT1(0x48);                         /* rex.w */
        PUT2A(0x8bb7, OFFC(cm));            /* mov rsi,[edi+&cm] */
        PUT3(0x8b048e);                     /* mov eax,[rsi+rcx*4] */
        PUT4(0x8b5c8e04);                   /* mov ebx,[rsi+rcx*4+4] */
        PUT3(0x83fa20);                     /* cmp edx,32 */
        PUT3(0x83d9ff);                     /* sbb ecx,-1 */
        PUT2A(0x898f, OFFC(cxt));           /* mov [edi+cxt],ecx */
        PUT3(0xc1e80a);                     /* shr eax,10 */
        PUT3(0xc1eb0a);                     /* shr ebx,10 */
        PUT2(0x29c3);                       /* sub ebx,eax */
        PUT3(0x0fafda);                     /* imul ebx,edx */
        PUT3(0xc1e006);                     /* shr eax,6 */
        PUT2(0x01d8);                       /* add eax,ebx */
        PUT3(0xc1e80d);                     /* shr eax,13 */
        PUT4A(0x0fbf8447, OFF(stretcht));   /* movsx eax,word[edi+rax*2+..] */
        PUT2A(0x8987, OFF_P(i));            /* mov [edi+&p[i]],eax */
        break;

      default:
        zq_error("invalid ZPAQ component");
    }
  }

  /* return squash(p[n-1]) */
  PUT2A(0x8b87, OFF_P(n - 1));              /* mov eax,[edi+&p[n-1]] */
  PUT1A(0x05, 0x800);                       /* add eax,2048 */
  PUT4A(0x0fbf8447, OFF(squasht));          /* movsx eax,word[edi+rax*2+..] */
  PUT1(0x5f);                               /* pop rdi */
  PUT1(0x5e);                               /* pop rsi */
  PUT1(0x5d);                               /* pop rbp */
  PUT1(0x5b);                               /* pop rbx */
  PUT1(0xc3);                               /* ret */

  /* Initialize for update(): predictor in rdi, bit y in rsi -> rbp. */
  save_o = o;
  o = 5;
  PUT1A(0xe9, save_o - 10);                 /* jmp update */
  o = save_o;
  PUT1(0x53);                               /* push rbx */
  PUT1(0x55);                               /* push rbp */
  PUT1(0x56);                               /* push rsi */
  PUT1(0x57);                               /* push rdi */
  PUT3(0x4889f5);                           /* mov rbp,rsi (2nd arg) */

  /* Code update() for each component */
  cp = hcomp + 7;
  for (i = 0; i < n; ++i, cp += zq_compsize[cp[0]]) {
    if (cp - hcomp >= pr->z->cend) zq_error("comp too big");
    if (cp[0] < 1 || cp[0] > 9) zq_error("invalid component");
    switch (cp[0]) {

      case ZQ_CONS:
        break;

      case ZQ_SSE:
      case ZQ_CM:
        /* train(cr, y):
           pn=cr.cm(cxt); count=pn&0x3ff; error=y*32767-(pn>>17);
           pn+=(error*dt[count]&-1024)+(count<cr.limit); */
        PUT1(0x48);                         /* rex.w */
        PUT2A(0x8bb7, OFFC(cm));            /* mov rsi,[edi+&cm] */
        PUT2A(0x8b87, OFFC(cxt));           /* mov eax,[edi+&cxt] */
        PUT1A(0x25, (int)(pr->comp[i].cm.n - 1)); /* and eax,size-1 */
        PUT1(0x48);                         /* rex.w */
        PUT3(0x8d3486);                     /* lea rsi,[rsi+rax*4] */
        PUT2(0x8b06);                       /* mov eax,[rsi] */
        PUT2(0x89c2);                       /* mov edx,eax */
        PUT3(0xc1e811);                     /* shr eax,17 */
        PUT2(0x89e9);                       /* mov ecx,ebp */
        PUT3(0xc1e10f);                     /* shl ecx,15 */
        PUT2(0x29e9);                       /* sub ecx,ebp ; y*32767 */
        PUT2(0x29c1);                       /* sub ecx,eax ; error */
        PUT2A(0x81e2, 0x3ff);               /* and edx,1023 ; count */
        PUT3A(0x8b8497, OFF(dt));           /* mov eax,[edi+rdx*4+&dt] */
        PUT3(0x0fafc8);                     /* imul ecx,eax */
        PUT2A(0x81e1, 0xfffffc00);          /* and ecx,-1024 */
        PUT2A(0x81fa, cp[2 + 2 * (cp[0] == ZQ_SSE)] * 4); /* cmp edx,limit */
        PUT2(0x110e);                       /* adc [rsi],ecx */
        break;

      case ZQ_ICM:
      case ZQ_ISSE:
        /* ht[c+(hmap4&15)]=st.next(...); ICM: adjust cm[cxt];
           ISSE: update the two weights. */
        PUT3(0x8b4700 + OFF(hmap4));        /* mov eax,[edi+&hmap4] */
        PUT3(0x83e00f);                     /* and eax,15 */
        PUT2A(0x0387, OFFC(c));             /* add eax,[edi+&c] */
        PUT1(0x48);                         /* rex.w */
        PUT2A(0x8bb7, OFFC(ht));            /* mov rsi,[edi+&ht] */
        PUT4(0x0fb61406);                   /* movzx edx,byte[rsi+rax] */
        PUT4(0x8d5c9500);                   /* lea ebx,[rbp+rdx*4] */
        PUT4A(0x0fb69c1f, OFF(st));         /* movzx ebx,byte[edi+rbx+&st] */
        PUT3(0x881c06);                     /* mov [rsi+rax],bl */
        PUT1(0x48);                         /* rex.w */
        PUT2A(0x8bb7, OFFC(cm));            /* mov rsi,[edi+&cm] */
        if (cp[0] == ZQ_ICM) {
          PUT1(0x48);                       /* rex.w */
          PUT3(0x8d3496);                   /* lea rsi,[rsi+rdx*4] */
          PUT2(0x8b06);                     /* mov eax,[rsi] */
          PUT3(0xc1e808);                   /* shr eax,8 */
          PUT2(0x89e9);                     /* mov ecx,ebp */
          PUT3(0xc1e10f);                   /* shl ecx,15 */
          PUT2(0x29e9);                     /* sub ecx,ebp */
          PUT2(0x29c1);                     /* sub ecx,eax */
          PUT3(0xc1f902);                   /* sar ecx,2 */
          PUT2(0x010e);                     /* add [rsi],ecx */
        } else {
          PUT2A(0x8b87, OFF_P(i));          /* mov eax,[edi+&p[i]] */
          PUT1A(0x05, 2048);                /* add eax,2048 */
          PUT4A(0x0fb78447, OFF(squasht));  /* movzx eax,word[edi+rax*2+..] */
          PUT2(0x89e9);                     /* mov ecx,ebp */
          PUT3(0xc1e10f);                   /* shl ecx,15 */
          PUT2(0x29e9);                     /* sub ecx,ebp */
          PUT2(0x29c1);                     /* sub ecx,eax ; err */
          PUT2A(0x8b87, OFF_P(cp[2]));      /* mov eax,[edi+&p[j]] */
          PUT3(0x0fafc1);                   /* imul eax,ecx */
          PUT1A(0x05, (1 << 12));           /* add eax,4096 */
          PUT3(0xc1f80d);                   /* sar eax,13 */
          PUT3(0x0304d6);                   /* add eax,[rsi+rdx*8] */
          PUT1A(0x3d, (1 << 19) - 1);       /* cmp eax,(1<<19)-1 */
          PUT2(0x7e05);                     /* jle L1 */
          PUT1A(0xb8, (1 << 19) - 1);       /* mov eax,(1<<19)-1 */
          PUT1A(0x3d, 0xfff80000);          /* cmp eax,-1<<19 */
          PUT2(0x7d05);                     /* jge L2 */
          PUT1A(0xb8, 0xfff80000);          /* mov eax,-1<<19 */
          PUT3(0x8904d6);                   /* L2: mov [rsi+rdx*8],eax */
          PUT3(0x83c110);                   /* add ecx,16 */
          PUT3(0xc1f905);                   /* sar ecx,5 */
          PUT4(0x034cd604);                 /* add ecx,[rsi+rdx*8+4] */
          PUT2A(0x81f9, (1 << 19) - 1);     /* cmp ecx,(1<<19)-1 */
          PUT2(0x7e05);                     /* jle L3 */
          PUT1A(0xb9, (1 << 19) - 1);       /* mov ecx,(1<<19)-1 */
          PUT2A(0x81f9, 0xfff80000);        /* cmp ecx,-1<<19 */
          PUT2(0x7d05);                     /* jge L4 */
          PUT1A(0xb9, 0xfff80000);          /* mov ecx,-1<<19 */
          PUT4(0x894cd604);                 /* L4: mov [rsi+rdx*8+4],ecx */
        }
        break;

      case ZQ_MATCH:
        /* if (c!=y) a=0; ht(limit)+=ht(limit)+y;
           if (++cxt==8) { cxt=0; ++limit; limit&=bufsize-1;
             if (a==0) { b=limit-cm(h[i]); if (b&(bufsize-1))
               while (a<255 && ht(limit-a-1)==ht(limit-a-b-1)) ++a; }
             else a+=(a<255);
             cm(h[i])=limit; } */
        PUT1(0x48);                         /* rex.w */
        PUT2A(0x8bb7, OFFC(ht));            /* mov rsi,[edi+&ht] */
        PUT1(0x48);                         /* rex.w */
        PUT2A(0x8b9f, OFFC(cm));            /* mov rbx,[edi+&cm] */
        PUT2A(0x8b87, OFFC(c));             /* mov eax,[edi+&c] */
        PUT2(0x39e8);                       /* cmp eax,ebp */
        PUT2(0x7408);                       /* jz L1 */
        PUT2(0x31c0);                       /* xor eax,eax */
        PUT2A(0x8987, OFFC(a));             /* mov [edi+&a],eax */
        PUT2A(0x8b87, OFFC(limit));         /* mov eax,[edi+&limit] */
        PUT4(0x0fb60c06);                   /* movzx ecx,byte[rsi+rax] */
        PUT2(0x01c9);                       /* add ecx,ecx */
        PUT2(0x01e9);                       /* add ecx,ebp */
        PUT3(0x880c06);                     /* mov [rsi+rax],cl */
        PUT2A(0x8b87, OFFC(cxt));           /* mov eax,[edi+&cxt] */
        PUT2(0xffc0);                       /* inc eax */
        PUT3(0x83e007);                     /* and eax,7 */
        PUT2A(0x8987, OFFC(cxt));           /* mov [edi+&cxt],eax */
        PUT2A(0x0f85, 0x9b);                /* jnz L8 */
        PUT2A(0x8b87, OFFC(limit));         /* mov eax,[edi+&limit] */
        PUT2(0xffc0);                       /* inc eax */
        PUT1A(0x25, (1 << cp[2]) - 1);      /* and eax,bufsize-1 */
        PUT2A(0x8987, OFFC(limit));         /* mov [edi+&limit],eax */
        PUT2A(0x8b87, OFFC(a));             /* mov eax,[edi+&a] */
        PUT2(0x85c0);                       /* test eax,eax */
        PUT2(0x755c);                       /* jnz L6 */
        PUT2A(0x8b8f, OFF_H(i));            /* mov ecx,[edi+&h[i]] */
        PUT2A(0x81e1, (1 << cp[1]) - 1);    /* and ecx,size-1 */
        PUT2A(0x8b87, OFFC(limit));         /* mov eax,[edi+&limit] */
        PUT3(0x2b048b);                     /* sub eax,[rbx+rcx*4] */
        PUT2A(0x8987, OFFC(b));             /* mov [edi+&b],eax */
        PUT1A(0xa9, (1 << cp[2]) - 1);      /* test eax,bufsize-1 */
        PUT2(0x7448);                       /* jz L7 */
        PUT1(0x53);                         /* push rbx */
        PUT2A(0x8b9f, OFFC(limit));         /* mov ebx,[edi+&limit] */
        PUT2(0x89da);                       /* mov edx,ebx */
        PUT2(0x29c3);                       /* sub ebx,eax ; limit-b */
        PUT2(0x31c9);                       /* xor ecx,ecx ; a=0 */
        PUT2A(0x81f9, 0xff);                /* L2: cmp ecx,255 */
        PUT2(0x741c);                       /* jz L3 */
        PUT2(0xffca);                       /* dec edx */
        PUT2(0xffcb);                       /* dec ebx */
        PUT2A(0x81e2, (1 << cp[2]) - 1);    /* and edx,bufsize-1 */
        PUT2A(0x81e3, (1 << cp[2]) - 1);    /* and ebx,bufsize-1 */
        PUT3(0x8a0416);                     /* mov al,[rsi+rdx] */
        PUT3(0x3a041e);                     /* cmp al,[rsi+rbx] */
        PUT2(0x7504);                       /* jnz L3 */
        PUT2(0xffc1);                       /* inc ecx */
        PUT2(0xebdc);                       /* jmp L2 */
        PUT1(0x5b);                         /* L3: pop rbx */
        PUT2A(0x898f, OFFC(a));             /* mov [edi+&a],ecx */
        PUT2(0xeb0e);                       /* jmp L7 */
        PUT1A(0x3d, 0xff);                  /* L6: cmp eax,255 */
        PUT3(0x83d000);                     /* adc eax,0 */
        PUT2A(0x8987, OFFC(a));             /* mov [edi+&a],eax */
        PUT2A(0x8b87, OFF_H(i));            /* L7: mov eax,[edi+&h[i]] */
        PUT1A(0x25, (1 << cp[1]) - 1);      /* and eax,size-1 */
        PUT2A(0x8b8f, OFFC(limit));         /* mov ecx,[edi+&limit] */
        PUT3(0x890c83);                     /* mov [rbx+rax*4],ecx */
        break;

      case ZQ_AVG:
        break;

      case ZQ_MIX2:
        /* err=(y*32767-squash(p[i]))*rate>>5;
           w=a16[cxt]; w+=(err*(p[j]-p[k])+4096)>>13; clamp w to 0..65535; */
        PUT2A(0x8b87, OFF_P(i));            /* mov eax,[edi+&p[i]] */
        PUT1A(0x05, 2048);                  /* add eax,2048 */
        PUT4A(0x0fb78447, OFF(squasht));    /* movzx eax,word[edi+rax*2+..] */
        PUT2(0x89e9);                       /* mov ecx,ebp */
        PUT3(0xc1e10f);                     /* shl ecx,15 */
        PUT2(0x29e9);                       /* sub ecx,ebp */
        PUT2(0x29c1);                       /* sub ecx,eax */
        PUT2A(0x69c9, cp[4]);               /* imul ecx,rate */
        PUT3(0xc1f905);                     /* sar ecx,5 */
        PUT2A(0x8b87, OFFC(cxt));           /* mov eax,[edi+&cxt] */
        PUT1(0x48);                         /* rex.w */
        PUT2A(0x8bb7, OFFC(a16));           /* mov rsi,[edi+&a16] */
        PUT1(0x48);                         /* rex.w */
        PUT3(0x8d3446);                     /* lea rsi,[rsi+rax*2] */
        PUT2A(0x8b87, OFF_P(cp[2]));        /* mov eax,[edi+&p[j]] */
        PUT2A(0x2b87, OFF_P(cp[3]));        /* sub eax,[edi+&p[k]] */
        PUT3(0x0fafc1);                     /* imul eax,ecx */
        PUT1A(0x05, 1 << 12);               /* add eax,4096 */
        PUT3(0xc1f80d);                     /* sar eax,13 */
        PUT3(0x0fb716);                     /* movzx edx,word[rsi] */
        PUT2(0x01d0);                       /* add eax,edx */
        PUT1A(0xba, 0xffff);                /* mov edx,65535 */
        PUT2(0x39d0);                       /* cmp eax,edx */
        PUT3(0x0f4fc2);                     /* cmovg eax,edx */
        PUT2(0x31d2);                       /* xor edx,edx */
        PUT2(0x39d0);                       /* cmp eax,edx */
        PUT3(0x0f4cc2);                     /* cmovl eax,edx */
        PUT3(0x668906);                     /* mov word [rsi],ax */
        break;

      case ZQ_MIX: {
        /* err=(y*32767-squash(p[i]))*rate>>4; wt=&cm[cxt];
           wt[j]=clamp512k(wt[j]+((err*p[j]+4096)>>13)); */
        int k;
        PUT2A(0x8b87, OFF_P(i));            /* mov eax,[edi+&p[i]] */
        PUT1A(0x05, 2048);                  /* add eax,2048 */
        PUT4A(0x0fb78447, OFF(squasht));    /* movzx eax,word[edi+rax*2+..] */
        PUT2(0x89e9);                       /* mov ecx,ebp */
        PUT3(0xc1e10f);                     /* shl ecx,15 */
        PUT2(0x29e9);                       /* sub ecx,ebp */
        PUT2(0x29c1);                       /* sub ecx,eax */
        PUT2A(0x69c9, cp[4]);               /* imul ecx,rate */
        PUT3(0xc1f904);                     /* sar ecx,4 */
        PUT2A(0x8b87, OFFC(cxt));           /* mov eax,[edi+&cxt] */
        PUT1(0x48);                         /* rex.w */
        PUT2A(0x8bb7, OFFC(cm));            /* mov rsi,[edi+&cm] */
        PUT1(0x48);                         /* rex.w */
        PUT3(0x8d3486);                     /* lea rsi,[rsi+rax*4] */
        for (k = 0; k < cp[3]; ++k) {
          PUT2A(0x8b87, OFF_P(cp[2] + k));  /* mov eax,[edi+&p[j+k]] */
          PUT3(0x0fafc1);                   /* imul eax,ecx */
          PUT1A(0x05, 1 << 12);             /* add eax,4096 */
          PUT3(0xc1f80d);                   /* sar eax,13 */
          PUT2(0x0306);                     /* add eax,[rsi] */
          PUT1A(0x3d, (1 << 19) - 1);       /* cmp eax,(1<<19)-1 */
          PUT2(0x7e05);                     /* jge L1 */
          PUT1A(0xb8, (1 << 19) - 1);       /* mov eax,(1<<19)-1 */
          PUT1A(0x3d, 0xfff80000);          /* cmp eax,-1<<19 */
          PUT2(0x7d05);                     /* jle L2 */
          PUT1A(0xb8, 0xfff80000);          /* mov eax,-1<<19 */
          PUT2(0x8906);                     /* L2: mov [rsi],eax */
          if (k < cp[3] - 1) {
            PUT1(0x48);                     /* rex.w */
            PUT3(0x83c604);                 /* add rsi,4 */
          }
        }
        break;
      }

      default:
        zq_error("invalid ZPAQ component");
    }
  }

  /* return from update() */
  PUT1(0x5f);                               /* pop rdi */
  PUT1(0x5e);                               /* pop rsi */
  PUT1(0x5d);                               /* pop rbp */
  PUT1(0x5b);                               /* pop rbx */
  PUT1(0xc3);                               /* ret */

  return o;
}

#endif /* ZQ_NOJIT */
