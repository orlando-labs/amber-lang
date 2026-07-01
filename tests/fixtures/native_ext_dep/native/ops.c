#include "ops.h"

AmberStatus amber_dep_doubled(AmberCtx *cx, const AmberValue *args,
                              size_t argc, AmberValue *out) {
  int64_t n = 0;
  if (argc != 1 || !amber_as_int(cx, args[0], &n)) {
    return amber_fault(cx, "TypeError", "doubled expects one Int argument");
  }
  *out = amber_make_int(cx, n * 2);
  return AMBER_OK;
}
