/* Foreign-handle backing for `native class Box` (native-packages 5c-ii test).
 * A Box owns a malloc'd counter; the handle's `owned` lifetime frees it via
 * box_box_free on destroy!. */
#include "runtime/amber_ext.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct {
  int64_t value;
} Box;

/* init(start) -> Box : constructor thunk, returns an owned handle. */
AmberStatus box_box_new(AmberCtx *cx, const AmberValue *args, size_t argc,
                        AmberValue *out) {
  int64_t start = 0;
  if (argc != 1 || !amber_as_int(cx, args[0], &start)) {
    return amber_fault(cx, "TypeError", "Box.init expects one Int");
  }
  Box *box = (Box *)malloc(sizeof(Box));
  if (box == NULL) {
    return amber_fault(cx, "RuntimeError", "out of memory");
  }
  box->value = start;
  *out = amber_make_handle(cx, "box.Box", box);
  return AMBER_OK;
}

/* bump!(by) -> self : mutates the foreign resource, returns the same handle. */
AmberStatus box_box_bump(AmberCtx *cx, AmberValue self, const AmberValue *args,
                         size_t argc, AmberValue *out) {
  void *handle = NULL;
  if (!amber_handle_ptr(cx, self, "box.Box", &handle)) {
    return AMBER_ERR;
  }
  int64_t by = 0;
  if (argc != 1 || !amber_as_int(cx, args[0], &by)) {
    return amber_fault(cx, "TypeError", "bump! expects one Int");
  }
  if (by == 0) {
    /* A thunk-raised, rescuable error (amber_fault maps to a `rescue`-able
     * class), exercised by the fixture's `rescue ValueError`. */
    return amber_fault(cx, "ValueError", "bump! by zero");
  }
  ((Box *)handle)->value += by;
  *out = self;
  return AMBER_OK;
}

/* value() -> Int */
AmberStatus box_box_value(AmberCtx *cx, AmberValue self, const AmberValue *args,
                          size_t argc, AmberValue *out) {
  (void)args;
  (void)argc;
  void *handle = NULL;
  if (!amber_handle_ptr(cx, self, "box.Box", &handle)) {
    return AMBER_ERR;
  }
  *out = amber_make_int(cx, ((Box *)handle)->value);
  return AMBER_OK;
}

/* destroy!() : owned destructor, full runtime context permitted (unused here). */
void box_box_free(AmberCtx *cx, void *handle) {
  (void)cx;
  free(handle);
}
