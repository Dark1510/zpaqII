/* zpaqII - state table and model components.
 * Port of libzpaq 7.15 (Unlicense, Matt Mahoney). */
#include "zq_internal.h"

#include <string.h>

void zq_st_init(zq_state_table *st) { memcpy(st->ns, zq_sns, sizeof(st->ns)); }

int zq_st_cminit(const zq_state_table *st, int state) {
  return ((st->ns[state * 4 + 3] * 2 + 1) << 22) /
         (st->ns[state * 4 + 2] + st->ns[state * 4 + 3] + 1);
}

void zq_component_init(zq_component *cr) {
  cr->limit = cr->cxt = cr->a = cr->b = cr->c = 0;
  zq_arr_resize(&cr->cm, 0, 4);
  zq_arr_resize(&cr->ht, 0, 1);
  zq_arr_resize(&cr->a16, 0, 2);
}
