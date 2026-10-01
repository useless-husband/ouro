// The optimisation pipeline.

#include "ouro.h"

void optimize(Func *f) {
  opt_simplify_cfg(f);
  opt_split_critical_edges(f);
  opt_loop_depth(f);
}
