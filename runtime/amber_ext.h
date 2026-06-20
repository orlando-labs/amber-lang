/*
 * amber_ext.h — stable C ABI for Amber native extension packages.
 *
 * This is the single contract a native extension package programs against. It
 * is plain C so an external author can compile their thunks without building
 * the Amber tree; in-tree, the runtime implements this same contract on top of
 * the StdlibHost facade (one marshalling implementation, two consumers — see
 * DESIGN-native-extension-packages-2026-06-20.md §6).
 *
 * Conventions:
 *  - `AmberValue` is opaque; never inspect it directly, use the readers and
 *    builders below. Values handed to a thunk (self, args) and values produced
 *    by builders are owned by the runtime and remain valid for the duration of
 *    the call only. Do not retain an `AmberValue` past the thunk's return.
 *  - Borrowed views (`amber_str_view`, `amber_bytes_view`, `amber_handle_ptr`)
 *    point into runtime-owned storage and are valid for the duration of the
 *    call only. Copy out anything you need to keep.
 *  - A thunk reports success by returning AMBER_OK with `*out` set, and failure
 *    by returning the result of `amber_fault(...)` (which records the fault on
 *    the active frame). On failure `*out` is ignored.
 */
#ifndef AMBER_EXT_H
#define AMBER_EXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABI version. The runtime rejects an extension whose
 * amber_ext_abi_version() does not match what the runtime was built with. */
#define AMBER_EXT_ABI_VERSION 1u

/* Opaque runtime call context (the active frame + host facade). */
typedef struct AmberCtx AmberCtx;

/* Opaque Amber value handle, valid only for the duration of the current call. */
typedef struct AmberValueObj *AmberValue;

/* Thunk outcome. AMBER_OK: `*out` holds the result. AMBER_ERR: a fault has been
 * recorded via amber_fault (or a propagating exception is pending). */
typedef enum AmberStatus {
  AMBER_OK = 0,
  AMBER_ERR = 1
} AmberStatus;

/* ---- value predicates (1 = yes, 0 = no) -------------------------------- */
int amber_is_null(AmberCtx *cx, AmberValue value);
int amber_is_bool(AmberCtx *cx, AmberValue value);
int amber_is_int(AmberCtx *cx, AmberValue value);
int amber_is_float(AmberCtx *cx, AmberValue value);
int amber_is_str(AmberCtx *cx, AmberValue value);
int amber_is_bytes(AmberCtx *cx, AmberValue value);
int amber_is_list(AmberCtx *cx, AmberValue value);
int amber_is_handle(AmberCtx *cx, AmberValue value);

/* ---- readers (1 = success, 0 = wrong kind; no fault is set) ------------- */
int amber_as_bool(AmberCtx *cx, AmberValue value, int *out);
int amber_as_int(AmberCtx *cx, AmberValue value, int64_t *out);
int amber_as_float(AmberCtx *cx, AmberValue value, double *out);

/* Borrowed UTF-8 text of a Str value. Valid for the duration of the call. */
int amber_str_view(AmberCtx *cx, AmberValue value, const char **ptr,
                   size_t *len);

/* Borrowed byte view of a Bytes value (zero-copy). Valid for the duration of
 * the call. */
int amber_bytes_view(AmberCtx *cx, AmberValue value, const uint8_t **ptr,
                     size_t *len);

size_t amber_list_len(AmberCtx *cx, AmberValue value);
AmberValue amber_list_at(AmberCtx *cx, AmberValue value, size_t index);

/* Recover the foreign pointer behind a `native class` handle. Verifies the
 * value is a handle tagged `tag` and is still live (tombstone check); on a tag
 * mismatch or use-after-destroy it records a fault and returns 0. */
int amber_handle_ptr(AmberCtx *cx, AmberValue value, const char *tag,
                     void **out);

/* ---- builders (return a value owned by the runtime for this call) ------- */
AmberValue amber_make_null(AmberCtx *cx);
AmberValue amber_make_bool(AmberCtx *cx, int value);
AmberValue amber_make_int(AmberCtx *cx, int64_t value);
AmberValue amber_make_float(AmberCtx *cx, double value);
AmberValue amber_make_str(AmberCtx *cx, const char *ptr, size_t len);
AmberValue amber_make_bytes(AmberCtx *cx, const uint8_t *ptr, size_t len);
AmberValue amber_make_list(AmberCtx *cx, const AmberValue *items, size_t count);

/* Wrap a foreign pointer as a `native class` handle tagged `tag`. The
 * ownership/reclaim declared for `tag` in the package manifest governs teardown
 * (deterministic destroy! for owned/collected; GC reclaim only for collected). */
AmberValue amber_make_handle(AmberCtx *cx, const char *tag, void *ptr);

/* ---- faults and callbacks --------------------------------------------- */
/* Record a fault (mapped to the rescuable error class named `error_class`,
 * e.g. "TypeError", "LifetimeError") and return AMBER_ERR for the thunk to
 * propagate. */
AmberStatus amber_fault(AmberCtx *cx, const char *error_class,
                        const char *message);

/* Invoke an Amber block value with `argc` arguments. On AMBER_OK `*out` holds
 * the block result; on AMBER_ERR a fault/exception is propagating. */
AmberStatus amber_call_block(AmberCtx *cx, AmberValue block,
                             const AmberValue *args, size_t argc,
                             AmberValue *out);

/* ---- thunk and lifetime function signatures --------------------------- */
/* Free function / constructor: `(args) -> out`. */
typedef AmberStatus (*AmberFreeFn)(AmberCtx *cx, const AmberValue *args,
                                   size_t argc, AmberValue *out);

/* Method on a handle: `(self, args) -> out`. */
typedef AmberStatus (*AmberMethodFn)(AmberCtx *cx, AmberValue self,
                                     const AmberValue *args, size_t argc,
                                     AmberValue *out);

/* Destructor for an `owned` handle: full runtime context permitted; invoked
 * only via deterministic destroy!/memory.dealloc, never by the GC. */
typedef void (*AmberOwnedDestructor)(AmberCtx *cx, void *handle);

/* Reclaim for a `collected` handle: context-free by construction so it cannot
 * reenter the runtime; may be invoked by deterministic destroy! OR by the GC. */
typedef void (*AmberCollectedReclaim)(void *handle);

/* ---- load-time handshake ---------------------------------------------- */
/* Every extension translation unit must export this, returning
 * AMBER_EXT_ABI_VERSION, so the runtime can reject an ABI mismatch at load. */
uint32_t amber_ext_abi_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AMBER_EXT_H */
