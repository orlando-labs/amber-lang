/* Native acceleration for `native def doubled` (native-packages 5c-ii test).
 * Deliberately returns a different value than the Amber fallback body
 * (n * 2, not n * 10) so a passing test proves the native binary calls this
 * thunk rather than running the bytecode body. */
#include "runtime/amber_ext.h"

AmberStatus amber_demo_doubled(AmberCtx *cx, const AmberValue *args,
                               size_t argc, AmberValue *out) {
  int64_t n = 0;
  if (argc != 1 || !amber_as_int(cx, args[0], &n)) {
    return amber_fault(cx, "TypeError", "doubled expects one Int argument");
  }
  *out = amber_make_int(cx, n * 2);
  return AMBER_OK;
}
