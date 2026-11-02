/* zpaqII - postprocessor (PCOMP programs).
 * Port of libzpaq 7.15 (Unlicense, Matt Mahoney). */
#include "zq_internal.h"

#include <string.h>

/* Copy ph, pm from the block header and reset the state machine. */
void zq_pp_open(zq_postproc *pp, int h, int m) {
  if (!pp->z.outbuf.data) zq_arr_resize(&pp->z.outbuf, 1 << 14, 1);
  pp->state = 0;
  pp->hsize = 0;
  pp->ph = h;
  pp->pm = m;
  zq_zpaql_clear(&pp->z);
}

void zq_pp_free(zq_postproc *pp) { zq_zpaql_free(&pp->z); }

void zq_pp_set_output(zq_postproc *pp, zq_writer *out) { pp->z.output = out; }
void zq_pp_set_sha1(zq_postproc *pp, zq_sha1 *sha1) { pp->z.sha1 = sha1; }
int zq_pp_state(const zq_postproc *pp) { return pp->state; }

/* (PASS=0 | PROG=1 psize[0..1] pcomp[0..psize-1]) data... EOB=-1
 * Return state: 1=PASS, 2..4=loading PROG, 5=PROG loaded. */
int zq_pp_write(zq_postproc *pp, int c) {
  switch (pp->state) {
    case 0:
      if (c < 0) zq_error("Unexpected EOS");
      pp->state = c + 1;
      if (pp->state > 2) zq_error("unknown post processing type");
      if (pp->state == 1) zq_zpaql_clear(&pp->z);
      break;
    case 1: /* PASS */
      zq_zpaql_outc(&pp->z, c);
      break;
    case 2: /* PROG */
      if (c < 0) zq_error("Unexpected EOS");
      pp->hsize = c;
      pp->state = 3;
      break;
    case 3: /* PROG psize[0] */
      if (c < 0) zq_error("Unexpected EOS");
      pp->hsize += c * 256;
      if (pp->hsize < 1) zq_error("Empty PCOMP");
      zq_arr_resize(&pp->z.header, (size_t)pp->hsize + 300, 1);
      pp->z.cend = 8;
      pp->z.hbegin = pp->z.hend = pp->z.cend + 128;
      ZQ_HDR(&pp->z, 4) = (zq_u8)pp->ph;
      ZQ_HDR(&pp->z, 5) = (zq_u8)pp->pm;
      pp->state = 4;
      break;
    case 4: /* PROG psize[0..1] pcomp[0...] */
      if (c < 0) zq_error("Unexpected EOS");
      ZQ_HDR(&pp->z, pp->z.hend++) = (zq_u8)c;
      if (pp->z.hend - pp->z.hbegin == pp->hsize) { /* last byte */
        const int hsize = pp->z.cend - 2 + pp->z.hend - pp->z.hbegin;
        ZQ_HDR(&pp->z, 0) = (zq_u8)(hsize & 255);
        ZQ_HDR(&pp->z, 1) = (zq_u8)(hsize >> 8);
        zq_zpaql_initp(&pp->z);
        pp->state = 5;
      }
      break;
    case 5: /* PROG ... data */
      zq_zpaql_run(&pp->z, (zq_u32)c);
      if (c < 0) zq_zpaql_flush(&pp->z);
      break;
  }
  return pp->state;
}
