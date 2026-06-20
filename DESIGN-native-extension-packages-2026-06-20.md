# Native Packages: Best-Effort Native Amber + Mixed Amber/C/C++ Extensions

Status: design, 2026-06-20.
Supersedes: `DESIGN-native-extension-packages-2026-06-17.md` (which framed the work
as staged v1/v2 layers; this revision drops the layering in favour of a single
author-facing contract).

## 1. Core idea

**Native is a build strategy for an ordinary Amber definition, not a second API
surface.**

A package is always defined by its Amber surface. Hand-written C/C++ is an
*optional implementation* of selected Amber definitions, never a parallel world.
The Amber fallback body is the dial that slides a definition across the whole
spectrum:

- a **pure-Amber package** is best-effort compiled to native, exactly the way the
  root module is today — there is nothing new to learn or declare;
- a **mixed package** pairs a pure-Amber reference body with a native
  acceleration; bytecode builds run the body, native builds call the symbol, and
  the two are proven observably identical;
- a **native-only leaf** (a wrapper over a foreign resource with no pure-Amber
  representation) is explicitly marked; it requires a native build and fails
  closed in bytecode.

The author never chooses "native or not" for ordinary code — the **build target**
does. The only author-level choice is marking the handful of definitions whose
identity *is* a foreign resource.

## 2. Design principles

1. **No version gates.** There is one author-facing contract: the surface, the
   manifest split, the runtime ABI, the lifetime rules, the equivalence
   discipline. Static linking is simply the first build strategy. A prebuilt
   object cache, prebuilt binary variants, and runtime dynamic loading are
   *transparent performance/packaging optimizations* that never change what an
   author writes. We freeze the contract, not a feature ladder.
2. **Portable by default.** Bytecode is the portable, sandboxable tier. Native is
   how you buy performance, opted into explicitly. Native code is never smuggled
   into a bytecode run (see §8.2).
3. **Explicit trust.** Building or running native code is gated by the existing
   `ffi` capability and an explicit consumer acknowledgement. A library author
   cannot escalate a consumer into running C.
4. **Deterministic lifetime.** Foreign resources are released by the language's
   explicit lifetime model (`destroy!` / `memory.dealloc`), not by implicit GC
   finalizers — with one carefully-bounded opt-in (`collected`, §7.4).
5. **Equivalence is the safety net.** Wherever a fast native path shadows a
   readable Amber path, the toolchain proves they match.

## 3. The execution spectrum

| Package definition is written as | Build target = bytecode | Build target = native |
|---|---|---|
| Pure Amber | runs the Amber | best-effort compiled (whole graph, §8) |
| `native def` with an Amber body | runs the Amber body | calls the C symbol; **must match the body** |
| Native-only leaf (`native def`/`native class`, no body) | `NativeRequiredError` (build-time if statically reachable) | calls the C symbol |

## 4. Author-facing surface

The binding lives **in the Amber source** as a contextual modifier keyword on an
ordinary definition (consistent with the existing `class_method def` / `prop` /
`attr` modifier idiom), plus a `from "<logical.name>"` clause. The manifest
(§5) resolves logical names to physical symbols and carries build facts. The
source says *which* definitions are native and *what* they bind to; the manifest
says *how to build* the native side. Nothing is declared in both places.

This surface was chosen over a `@native(...)` decorator because `@` is the
instance-variable sigil in Amber (`@x`, `@@classvar`) and the language has no
decorator syntax; a leading modifier keyword is idiomatic and collision-free.

### 4.1 Free-function acceleration

```amber
# The body IS the bytecode fallback AND the spec the native symbol must match.
native def hash(data: Bytes) -> Bytes from "blake3.hash":
  state = blake3_iv()
  data.each_chunk(64) |block|:
    state = blake3_compress(state, block)
  blake3_finalize(state)
```

### 4.2 Foreign-handle types

A `native class` is a type whose instances wrap a foreign pointer. It reuses
Amber's real constructor (`init`) and deterministic destructor (`destroy!`) —
there are no `constructor`/`destructor`/`struct` keywords (the prior doc's
`native struct` is dropped: Amber has no `struct`).

```amber
# `owned` => Amber drives deterministic teardown via destroy! (see §7).
native class Hasher from "blake3.Hasher" owned:
  def init()                          from "blake3.hasher_new"
  def update!(data: Bytes) -> self    from "blake3.hasher_update"
  def finalize() -> Bytes             from "blake3.hasher_finalize"
  def destroy!()                      from "blake3.hasher_free"   # required by `owned`
```

Usage reads like ordinary Amber:

```amber
h = Hasher.new()
chunks.each |c|: h.update!(c)
digest = h.finalize()
h.destroy!()                          # deterministic; or memory.dealloc(h)
```

### 4.3 The mandatory-fallback rule

Every native binding **must** carry a pure-Amber reference body, **except**
definitions explicitly marked native-only. Two consequences:

- a `native def` with a body is portable: it works in bytecode and is
  equivalence-checked against its native symbol;
- a fallback body **must not depend on a native-only leaf** — otherwise the
  "works in bytecode" guarantee is hollow. (`hash` above is self-contained; it
  does not use `Hasher`.)

Foreign-handle types are inherently native-only: there is no pure-Amber value
that *is* a `blake3_hasher*`. So `native class Hasher` is a leaf — touching it in
a bytecode build raises `NativeRequiredError`, while `hash(data)` keeps working
everywhere. Same package, two points on the dial.

### 4.4 Contextual-keyword rules

`native`, `owned`, `borrowed`, and `collected` are **never reserved**. The lexer
emits them as ordinary identifiers; the parser recognizes them only by text in a
single position each. This guarantees no existing identifier breaks.

- **`native`** is a definition modifier only when it leads a statement and is
  immediately followed by `def` or `class`. Elsewhere (`native = 3`,
  `obj.native`) it is an identifier.
- **`owned` / `borrowed` / `collected`** are ownership markers only in the marker
  slot of a `native class` header — after the `from <StringLit>` clause, before
  the body-opening `:`:

  ```
  native-class-header := "native" "class" Name ["<" Super] "from" StringLit ownership ":"
  ownership           := "owned" | "borrowed" | "collected"
  ```

  Everywhere else (`owned = true`, `def transfer(owned)`, `@owned`,
  `account.owned?`) they are identifiers. The marker is **required** — there is
  no implicit default; foreign-resource ownership is never decided by omission.
  (`E_NATIVE_CLASS_OWNERSHIP_REQUIRED` on omission.) `native def` free functions
  take no ownership marker.

## 5. Manifest surface

Native build facts live in the package manifest, keyed by the logical names the
source already declared. The manifest never repeats `amber_name`/`signature`.

```toml
[[native]]
name         = "blake3"
language     = "c"                 # or "c++"
sources      = ["native/blake3.c", "native/amber_blake3.c"]
include_dirs = ["native/include"]
# link_libraries = ["blake3"]      # alternative to vendoring sources
cxxflags     = ["-O3"]             # allowlisted; folded into the package digest
capabilities = ["ffi"]            # consumer must grant ffi to build native

[native.symbols]                   # logical name (in source)  ->  physical C symbol
"blake3.hash"            = "amber_blake3_hash"
"blake3.hasher_new"      = "amber_blake3_hasher_new"
"blake3.hasher_update"   = "amber_blake3_hasher_update"
"blake3.hasher_finalize" = "amber_blake3_hasher_finalize"
"blake3.hasher_free"     = "amber_blake3_hasher_free"

[[native.types]]                   # dispatch identity (see §6)
amber      = "crypto.blake3.Hasher"
tag        = "blake3.Hasher"
ownership  = "owned"
destructor = "blake3.hasher_free"
```

Two manifests exist today and must be reconciled: the package manifest
(`amber.toml`, hand-rolled line parser in `package/package.cpp`) and the build
manifest (`amber.build.json`, JSON parser in `buildsys/build.cpp`, which already
drives the native build and already carries `native_eligible` /
`native_fallback_reason`). Native sections are *authored* in the package manifest
and *lowered* into the build manifest; the native build reads the lowered form.
The line-based TOML parser handles flat string arrays today but not nested
repeated tables well; express the binding manifest with the structured shape
above (or in JSON, matching `amber.build.json`) rather than extending the line
parser.

## 6. Runtime ABI contract

There is **one** value-marshalling contract, exposed two ways:

- in-tree, it is the existing `StdlibHost` facade (`runtime/stdlib_registry.h`):
  type-erased frame, value readers/builders, fault reporting, block calls,
  fs/random/time hosts;
- out-of-tree, it is a stable, documented, header-only **C** surface,
  `runtime/amber_ext.h`, that an external author compiles against without
  building the Amber tree.

`amber_ext.h` is the canonical contract; `StdlibHost` is its in-tree
implementation (one marshalling implementation, two consumers). The C surface is
justified here by a *present* need — external authors program against a header —
not by a future dlopen story.

It exposes:

- opaque `AmberCtx`, `AmberValue`, `AmberStatus`, an ABI-version constant and an
  init handshake;
- readers for `Null/Bool/Int/Float/Str/Bytes/array/map`, including a **zero-copy
  borrowed `Bytes` view** (`amber_bytes_view`, backed by the existing
  `RuntimePinViewKind::ValueBuffer`) so hashing/compression do not copy;
- builders for the same safe value set (`amber_make_bytes`, `amber_make_str`,
  `amber_make_list`, `amber_make_object`, …);
- fault reporting that maps to Amber's rescuable exception classes (reuse the
  `runtime_errors.def` X-macro), e.g. `amber_fault(cx, "TypeError", msg)`;
- foreign-handle operations: `amber_make_handle(cx, tag, ptr)` and
  `amber_handle_ptr(cx, self, tag, &out)` (the latter performs the tombstone
  check and faults on use-after-`destroy!`);
- block invocation for callbacks (`amber_call_block`), with the safepoint/root-map
  interaction defined for the extension frame.

Thunk signatures:

```c
// free function: (Bytes) -> Bytes
AmberStatus amber_blake3_hash(AmberCtx *cx, const AmberValue *args,
                              size_t argc, AmberValue *out);

// init() -> Hasher : returns an owned handle tagged blake3.Hasher
AmberStatus amber_blake3_hasher_new(AmberCtx *cx, const AmberValue *args,
                                    size_t argc, AmberValue *out);

// update!(self, Bytes) -> self : effectful; native-only leaf
AmberStatus amber_blake3_hasher_update(AmberCtx *cx, AmberValue self,
                                       const AmberValue *args, size_t argc,
                                       AmberValue *out);

// destructor for `owned`: full runtime context permitted
void amber_blake3_hasher_free(AmberCtx *cx, void *handle);
```

C++ packages expose the same `extern "C"` thunk surface and may use real C++
internally; the exported symbols stay C-compatible and versioned.

## 7. Ownership and lifetime

### 7.1 The three markers

- **`borrowed`** — Amber holds a non-owning handle and never frees it; the
  provider owns the lifetime. A `destroy!` binding is forbidden.
- **`owned`** — Amber owns the handle and releases it through the deterministic
  lifetime model only. Requires a `destroy!` binding. The GC **never** runs the
  destructor (§7.2). Forgetting to release leaks the foreign resource and trips a
  diagnostic backstop (§7.3).
- **`collected`** — `owned` plus opt-in GC reclamation (§7.4), for resources that
  are genuinely safe to free from the collector.

### 7.2 Why `owned` never uses a GC finalizer

This is normative in the lifetime spec (Part VII): the GC must not call a user
destructor automatically; deterministic cleanup happens only via explicit code or
an explicit runtime intrinsic, never "implicit finalizer magic." The reasons it
would be *unsound* in Amber's model, not merely undesirable:

1. **Wrong strand.** Objects have an owner strand and `destroy!` runs only on it.
   The GC runs on the collector strand, possibly with the world stopped. A native
   destructor firing there violates the owner-strand contract that the no-GIL
   design depends on.
2. **Reentrancy / resurrection.** A destructor runs arbitrary code mid-cycle. It
   can allocate (re-entering the GC), resurrect `self`, touch a sibling being
   collected in the same cycle (use-after-free), throw with no handler, or
   deadlock on a stopped-world lock. GC correctness assumes a frozen, consistent
   heap.
3. **Timing.** The GC runs late, or never (process exit). Scarce resources (fds,
   sockets, kernel handles) are exhausted long before memory pressure triggers a
   collection.
4. **Ordering.** Among dying handle-objects the GC gives no order, so an object
   that frees a resource a neighbour still references can break it.

`destroy!` avoids all four: it runs at a known program point, on the owner strand,
with the normal exception path, transitioning the object through observable
tombstone states (`live → destroyed → deallocated`, introspectable via
`memory.alive?` / `memory.destroyed?`).

### 7.3 The leak backstop (`owned`)

When the GC reclaims an `owned` handle-object whose handle slot is still live, the
runtime **detects and reports** the orphan (dev-time warning, leak counter,
debug-mode abort) — it does **not** run the C destructor, because the bound symbol
is opaque (it might flush buffers, close fds, touch shared state, or call back
into Amber, all on the wrong strand mid-cycle). The backstop makes the bug
visible; it never guesses it is safe to free.

Ergonomics: deterministic cleanup is made hard to forget without involving the GC
— `ensure`/finalization clauses and block-scoped helpers in the
`memory.borrow(obj) |view|:` family call `destroy!` at predictable scope exits.

### 7.4 `collected` — opt-in GC reclamation, made sound

The danger in §7.2 is a property of the *resource*, not of native objects in
general. A `malloc`'d POD buffer or a thread-agnostic in-process object is safe to
free from the collector. `collected` is the explicit opt-in, kept sound by two
mechanical guardrails plus two author assertions.

**Guardrail 1 — restricted reclaim signature.** A `collected` type's destructor is
**context-free**: it receives only the raw handle, no `AmberCtx`.

```c
void z_buf_free(void *handle);     // no AmberCtx, by construction
```

With no door back into the runtime, the reentrancy / resurrection /
touch-a-sibling failures are *structurally impossible* regardless of strand. If
teardown needs runtime context (release child Amber objects, run a rich `destroy!`
body), it does not fit this signature — which is exactly the signal that the type
is `owned`, not `collected`. (`E_NATIVE_COLLECTED_RECLAIM_SIGNATURE` if an
`AmberCtx`-taking destructor is declared on a `collected` type.)

**Guardrail 2 — tombstone-gated once-only.** Explicit `destroy!` and GC
reclamation are mutually exclusive: whichever fires first flips the tombstone and
clears the handle slot; the other becomes a no-op. The runtime guarantees
once-only, so the reclaim function need not be idempotent and there is no
double-free across the two paths.

**Author assertions (FFI trust, unverifiable — part of the `ffi` grant):**

1. the reclaim is **thread-agnostic** (safe on the collector strand);
2. the resource **tolerates delayed-or-never release** (memory-like, not a scarce
   kernel handle).

If either is false, the type stays `owned`. Decision rule:

| The native resource is… | Marker |
|---|---|
| `malloc`'d buffer / in-process refcounted object; thread-agnostic free; OK if freed late or never | `collected` |
| fd, socket, mutex, GPU/device handle; thread-affine teardown; must release promptly | `owned` |
| owned by the provider; Amber must never free it | `borrowed` |

Caveat: process-exit GC sweeps do not reliably run reclaimers. `collected` means
"freed promptly-ish, or reclaimed by the OS at exit." If a resource *must* be
released before exit (flush-on-close, a cross-process lock), it is `owned` + a
scope/`ensure`, never `collected`.

## 8. Build pipeline

### 8.1 Best-effort native over the whole graph

Today the native build covers only the root module: `run_build_command`
(`tools/amberc/main.cpp`) builds a native executable from a single root artifact,
and the generated `run_vm_entry()` runs one `RuntimeWorld` over the root bytecode
without the module loader. A multi-module program is therefore not even fully
native today.

The native build must operate over the **linked module graph**. Recommended,
lowest-risk approach: run the module loader's resolution at build time
(`RuntimeModuleLoader` already computes export-cell bindings and init order), bake
the resolved init order and export bindings into a **single merged `BcModule`**,
and feed that to the existing per-code-object emitter unchanged. (Per-module
native objects + a native linker is a possible later optimization, not required.)

"Best-effort" then means, for a given target:

- pure-Amber code objects → natively lowered where eligible, else the per-function
  VM bridge, else whole-program restart (existing machinery, now graph-wide);
- `native def` with a body → native build uses the symbol, bytecode build uses the
  body;
- native-only leaf → native build uses the symbol; bytecode build raises
  `NativeRequiredError` (build-time diagnostic when statically reachable).

Per-artifact reporting already exists (`native_eligible`,
`native_fallback_reason`, native/vm/total code counts) and is extended with
extension digests, ABI version, and target triple.

### 8.2 Native-by-default ergonomics (no auto-bridge)

The runtime never auto-bridges a native-only type into a bytecode run: doing so
would require either compiling C at load (defeating the no-build-step, reproducible
bytecode tier) or shipping+`dlopen`-ing prebuilt platform binaries (a trust
escalation and a runtime gamble), and would silently erase the `ffi` decision.

Instead, the *common path is a native build*: `amber build` defaults to native
when the dependency graph carries native sources and `ffi` is granted, backed by
the object cache (§9) so it is fast. "It just works" is achieved by *being native,
explicitly* — `NativeRequiredError` appears only when a portable bytecode artifact
is deliberately requested, where a foreign resource genuinely cannot follow.

## 9. Distribution and optimization layers

Packages ship **source + digests**: Amber sources/bytecode, optional native
sources/headers, declared link libraries and allowlisted flags, the binding map,
capability requirements, and the equivalence contract. No build scripts, no
prebuilt blobs required, no runtime `dlopen` of arbitrary code in the base model.

The package digest covers Amber bytecode digests, native source/header digests,
the binding-manifest text, the resolved (allowlisted) compiler/link flags, the
compiler identity+version, the target triple, and the ABI version. The compiled
binary's own hash is **never** part of the package digest (it is host-specific and
non-reproducible).

These are pure performance/packaging optimizations layered under the same
contract, in order of likely arrival, none changing what an author writes:

1. **object cache** keyed by `{source digest, flags, compiler id+version, target
   triple, ABI version}` — makes native builds incremental/fast;
2. **prebuilt binary variants** a registry may publish for common triples;
3. **runtime dynamic loading** behind the same `amber_ext.h` ABI.

## 10. Backend-equivalence and soundness

### 10.1 Equivalence as the package contract

A definition with both an Amber body and a native binding is gated by the
`make backend-equivalence` discipline, extended to packages: the package's own
test corpus runs in both modes (Amber body vs. native symbol) and observable
output is diffed. Native-only leaves are exempt but marked and effect-typed.

### 10.2 Effectful bindings under whole-program restart

The native lane's soundness rests on "re-run the whole program on bailout stays
byte-identical" (`tools/amberc/main.cpp`), which holds only for side-effect-free
code. Therefore:

- pure `native def` accelerations (and their Amber bodies) may be VM-bridged and
  restarted freely;
- an **effectful** native binding is always a **native-only leaf** (e.g. a
  foreign-handle `update!`) — it has no Amber body, hence no VM-bridge target, so
  it is always a direct native call;
- the runtime must not place a restart boundary *after* an effect: an effectful
  native-only call is the bailout boundary itself.

This keeps the existing purity invariant intact: nothing that can be replayed by a
restart is allowed to carry an effect.

## 11. Dispatch identity

Native dispatch keys on the fixed `RuntimeNativeTypeKind` enum (`runtime/vm.h`),
which third-party types cannot extend. Foreign-handle types get a generic
`Foreign` kind carrying a per-`(package, type)` **tag** (`AMBER_TAG("blake3.Hasher")`,
declared in `[[native.types]]`), resolved through a per-build registry. The fixed
enum never grows for third-party types. This is distinct from the existing
opaque-handle/pin system (`RuntimeOpaqueHandle`), which exports Amber values *to*
native code; foreign handles are the inbound direction (a `Value` wrapping a host
pointer + tag + tombstone state) and are new.

## 12. Diagnostics

- `NativeRequiredError` — a native-only leaf was reached in a bytecode build.
- `E_NATIVE_CLASS_OWNERSHIP_REQUIRED` — `native class` header without a marker.
- `E_NATIVE_COLLECTED_RECLAIM_SIGNATURE` — `collected` type declares an
  `AmberCtx`-taking destructor.
- `LifetimeError` — handle used after `destroy!` (tombstone check in
  `amber_handle_ptr`).
- native-build symbol-presence check: every declared physical symbol is verified
  present in the compiled objects before link, failing early with a binding
  diagnostic rather than a raw linker error.
- ABI-version / target-triple / symbol-table mismatch on frozen-image load.

## 13. Tests

- contextual-keyword non-regression: `native`/`owned`/`borrowed`/`collected` as
  ordinary identifiers; as markers only in their slots;
- free-function acceleration: bytecode body vs. native symbol equivalence;
- `native class` round-trip: `init` → `update!` (effect) → `finalize` → `destroy!`,
  tombstone use-after-`destroy!` faults;
- `owned` leak backstop fires (detect, no free) on dropped-without-`destroy!`;
- `collected` GC reclamation runs the context-free reclaim once; tombstone gating
  prevents double-free against an explicit `destroy!`;
- `collected` with an `AmberCtx` destructor is rejected;
- fallback-depends-on-leaf is rejected;
- whole-graph native: a multi-module program builds native end to end with
  `vm_fallback_code_count = 0` where expected;
- deterministic package digest with native source blobs;
- missing symbol; ABI version mismatch; bytecode-only build of a required leaf;
- equivalence between a pure-Amber implementation and a native-backed
  implementation of the same exported API.

Run:

```sh
make test
make backend-equivalence
```

## 14. Implementation map

- **frontend** — contextual `native` modifier on `def`/`class` (lexer keeps tokens
  as identifiers; parser recognizes by position, mirroring the `next` precedent);
  `from <StringLit>` binding clause; ownership marker slot; new AST/HIR nodes;
  binder routes logical names; checker enforces mandatory-body + fallback-independence
  + effect typing of leaves.
- **runtime** — `amber_ext.h` C contract over the `StdlibHost` implementation;
  foreign-handle `Value` kind + tombstone integration with the lifetime model;
  per-build `Foreign` type-tag registry; `collected` reclaim scheduling off the GC.
- **buildsys / amberc** — whole-graph native via merged `BcModule`; lower
  `[[native]]` from package manifest into the build manifest; compile/link native
  sources with the allowlisted flags; symbol-presence check; extended reporting +
  digests.
- **package** — manifest schema for `[[native]]` / `[native.symbols]` /
  `[[native.types]]`; source/header blobs + digests; capability requirements.
- **frozen** — native extension metadata (package/module identity, extension name,
  ABI version, source-digest summary, target triple, exported-symbol-table digest,
  ownership/reclaim declarations); load-time mismatch rejection.

## 15. Decisions still open

- **`collected` naming** — `collected` vs `gc` vs `managed`. Recommend `collected`.
- **Marker default** — currently required (no default). Recommend keeping it
  required; the alternative is defaulting to `owned`.
- **Whole-graph approach** — merged single `BcModule` (recommended) vs. per-module
  native objects + a native linker (deferred optimization).

## 16. Corrections vs the 2026-06-17 draft

- dropped the v1/v2/vN layering for a single contract (§2);
- binding lives in source as a contextual modifier, not in the manifest and not as
  a `@native` decorator (§4);
- `native class`, not `native struct` (Amber has no `struct`); reuse `init` /
  `destroy!`, not new `constructor` / `destructor` keywords (§4.2);
- `owned` binds to deterministic `destroy!`, never a GC finalizer (§7.2); added
  `collected` as the bounded opt-in (§7.4);
- one ABI contract (`amber_ext.h` over `StdlibHost`), not a parallel marshalling
  layer (§6);
- explicit `ffi` capability gating and dispatch-identity tagging (§11, §2.3);
- effectful-binding/restart soundness rule made normative (§10.2).
```
