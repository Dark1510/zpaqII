/* zpaqII - ZPAQL disassembler and verifier (ZPAQL(II) tooling).
 *
 *   zdis -m x4               disassemble the config of a method string
 *                            (the internal form: 0, x4, s4.3, i1, ...)
 *   zdis file.conf           disassemble a config text ("comp ..." + assembly)
 *
 * It shows the HCOMP and PCOMP programs with their offsets, and checks what a
 * verifier can check statically: jump targets inside the program and operand
 * bytes that are actually present.  Read-only: it never writes an archive.
 */
#include "zq_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How many operand bytes an opcode takes (255 = LJ takes two). */
static int operand_bytes(int op) {
  if (op == 255) return 2;
  switch (op) {
    case 7: case 15: case 23: case 31:      /* x=r N */
    case 39: case 47: case 63:              /* JT/JF/JMP N */
    case 55:                                /* R=A N */
      return 1;
    default: break;
  }
  if (op >= 64 && op <= 119) return (op & 7) == 7;   /* dst= N */
  if (op >= 128 && op <= 239) return (op & 7) == 7;  /* A op= N */
  return 0;
}

static int is_jump(int op) { return op == 39 || op == 47 || op == 63; }

/* Signed 8-bit displacement, as the interpreter computes it. */
static int rel8(int b) { return ((b + 128) & 255) - 127; }

static void disasm(const char *what, const zq_u8 *code, int begin, int end) {
  int pc = begin;
  int bad = 0;

  printf("\n%s  (%d bytes, offsets %d..%d)\n", what, end - begin, begin, end - 1);
  if (end <= begin) {
    printf("  (empty)\n");
    return;
  }
  while (pc < end) {
    const int at = pc;
    const int op = code[pc];
    const char *name = zq_opcodelist[op & 255];
    const int nargs = operand_bytes(op);
    int i;

    printf("  %5d  %02x", at, op);
    for (i = 1; i <= nargs; ++i) {
      if (at + i < end) printf(" %02x", code[at + i]);
      else printf(" ??");
    }
    for (i = nargs; i < 2; ++i) printf("   ");

    if (name[0] == 0) {
      printf("  <invalid opcode 0x%02x>\n", op);
      ++bad;
      ++pc;
      continue;
    }
    printf("  %s", name);

    if (is_jump(op)) {
      if (at + 1 >= end) {
        printf("  <operand past the end>");
        ++bad;
        ++pc;
        continue;
      }
      {
        const int target = at + 1 + rel8(code[at + 1]);
        printf(" %d", rel8(code[at + 1]));
        if (target < begin || target >= end) {
          printf("   <jump outside the program: %d>", target);
          ++bad;
        }
      }
    } else if (op == 255) {
      if (at + 2 >= end) {
        printf("  <operand past the end>");
        ++bad;
        ++pc;
        continue;
      }
      {
        const int target = code[at + 1] + 256 * code[at + 2];
        printf(" %d", target);
        if (target < begin || target >= end) {
          printf("   <jump outside the program: %d>", target);
          ++bad;
        }
      }
    } else if (nargs > 0) {
      if (at + nargs >= end) {
        printf("  <operand past the end>");
        ++bad;
        ++pc;
        continue;
      }
      printf(" %d", code[at + 1]);
    }
    printf("\n");
    pc = at + 1 + nargs;
  }
  printf("  -> %d problem(s)\n", bad);
}

static char *read_file(const char *fn) {
  FILE *f = fopen(fn, "rb");
  char *buf;
  long n;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) {
    fclose(f);
    return NULL;
  }
  buf = (char *)malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    fclose(f);
    return NULL;
  }
  buf[n] = 0;
  fclose(f);
  return buf;
}

int main(int argc, char **argv) {
  char *cfg = NULL;
  int args[9] = {0};
  zq_zpaql hz, pz;
  const char *method = NULL;
  const char *file = NULL;
  int i;

  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) method = argv[++i];
    else file = argv[i];
  }

  if (!method && !file) {
    fprintf(stderr, "usage: zdis -m METHOD | zdis file.conf\n");
    return 2;
  }

  if (method) {
    cfg = zq_make_config(method, args);
    if (!cfg) {
      fprintf(stderr, "zdis: unknown method '%s'\n", method);
      return 2;
    }
    printf("config for method %s:\n%s\n", method, cfg);
  } else {
    cfg = read_file(file);
    if (!cfg) {
      fprintf(stderr, "zdis: cannot read %s\n", file);
      return 2;
    }
  }

  memset(&hz, 0, sizeof(hz));
  memset(&pz, 0, sizeof(pz));
  zq_compile(cfg, args, &hz, &pz, NULL);

  disasm("HCOMP", hz.header.data, hz.hbegin, hz.hend);
  disasm("PCOMP", pz.header.data, pz.hbegin, pz.hend);
  printf("\nH: 2^%d words, M: 2^%d bytes\n", ZQ_HDR(&hz, 1), ZQ_HDR(&hz, 2));

  free(cfg);
  return 0;
}
