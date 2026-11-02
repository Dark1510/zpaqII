/* zpaqII - ZPAQL JIT scaffolding (x86-64).
 *
 * Upstream compiles each HCOMP/PCOMP program to machine code once per block
 * (ZPAQL::assemble) and runs it per input byte.  This module owns the
 * executable buffer and the entry point; the emitter itself is not ported yet,
 * so zq_zpaql_assemble() reports "no code" and ZPAQL::run falls back to the
 * interpreter.  The contract below matches upstream:
 *
 *   rcode[0..9]      entry stub emitted by the assembler (not used yet)
 *   return value     0 = OK, 1 = bad opcode, 2 = out of memory,
 *                    3 = write error, other = ZPAQL execution error
 */
#include "zq_internal.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#ifndef ZQ_NOJIT

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

/* Compile z's current program into z->rcode.  Returns the number of bytes
 * needed (0 when the JIT is unavailable for this build/CPU). */
int zq_zpaql_assemble(zq_zpaql *z) {
  (void)z;
  return 0;
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
