# SKETCH: "Ember" — a web framework DSL for Amber

Date: 2026-06-29
Status: first-approximation sketch — exploratory, not a spec. Depends on the
macro system proposed in `DESIGN-macro-system-2026-06-29.md` (none of which is
implemented yet). Surface syntax here is illustrative and will move.
Scope: shows what an idiomatic Amber web framework DSL could look like and,
for each construct, **what it expands to** after macro expansion (F1.5), so the
DSL stays typed and zero-cost rather than riding the reflective MOP slow path.
Builds on: the existing `net.http` runtime (`runtime/net_http*.{h,cpp}`,
`net.http.Client`/`Response`/`Headers`/`Request`, capability-gated `net.connect`
— see the net.http memory) plus an assumed `net.http` *server* surface in the
same shape; the capability model; the block-suffix surface; and the four macro
trigger surfaces (§8 of the macro doc).

Working name: **Ember** (Amber + web). Placeholder.

---

## 1. Design stance

- **DSL = macros, not `method_missing`.** Every declarative form below expands,
  at compile time, into ordinary typed Amber: a route table built from typed
  handler `def`s, struct-typed params, explicit response values. There is **no
  runtime metaobject magic on the hot path** — routing is a built table, not a
  `method_missing` dispatch. (Contrast: a Rails-style `method_missing` router is
  an `Any`-boundary per Q4; see the macro doc §1 table.)
- **Capability-honest.** A server holds an explicit `net.listen` capability; it
  is passed in, never ambient. Handlers that touch the filesystem or call out to
  other services receive those capabilities explicitly.
- **Block-suffix first.** The surface leans on Amber's block-suffix syntax
  (`route ... :`) rather than Ruby-style `instance_eval` blocks, because Q3
  keeps `instance_eval` out and macros make it unnecessary.
- **Strand-friendly.** Each request is handled on a strand; the framework keeps
  request-scoped state strand-confined and shares only the immutable route table
  + frozen config across strands (matches the no-GIL isolation model).

## 2. Hello world

```amber
import ember

app = ember.App(name: "demo")

@route(GET, "/")
def index(req):
  Response.ok("hello, amber")

app.serve(listen)        # `listen` is a net.listen capability
```

`@route(GET, "/")` is an **attribute macro** (macro doc §8.3). It does not wrap
`index` in a closure at runtime — it expands, at compile time, into a typed
handler plus a registration entry:

```amber
# --- after macro expansion (illustrative) ---
def index(req: ember.Request) -> ember.Response:
  Response.ok("hello, amber")

ember.__register_route(__ember_routes,
  method: GET, path: "/", handler: &index)
```

The route table `__ember_routes` is a compile-time-seeded value the `App`
reads; dispatch is a typed table lookup, not reflection.

## 3. Path params, typed

```amber
@route(GET, "/users/:id")
def show_user(req, id: Int):
  user = Users.find(id)
  case user:
    Some(u): Response.json(u)
    None:    Response.not_found()
```

The macro reads the `:id` segment in the path literal and the `id: Int`
parameter, and emits a typed extraction + parse-or-400 prologue. Expansion:

```amber
def show_user(req: ember.Request) -> ember.Response:
  id = case Int.parse(req.path_param("id")):
    Ok(v):  v
    Err(_): return Response.bad_request("invalid id")
  user = Users.find(id)
  case user:
    Some(u): Response.json(u)
    None:    Response.not_found()
```

Because the parse + 400 is *generated*, the handler body sees a real `Int`. A
`method_missing` router could never give the body a typed `id` without a runtime
cast at an `Any`-boundary.

## 4. Grouped routes (block-suffix macro)

```amber
app.routes:
  group "/api/v1", before: [auth, rate_limit]:
    get  "/posts"        -> posts.index
    get  "/posts/:id"    -> posts.show
    post "/posts"        -> posts.create
    delete "/posts/:id"  -> posts.destroy
```

`routes:` and `group ... :` are **block-suffix macros** (macro doc §8.2). The
whole block is consumed as AST at compile time and flattened into registration
calls — no nested-closure runtime cost, prefixes concatenated statically:

```amber
ember.__register_route(t, method: GET,    path: "/api/v1/posts",
  handler: &posts.index,  before: [auth, rate_limit])
ember.__register_route(t, method: GET,    path: "/api/v1/posts/:id",
  handler: &posts.show,   before: [auth, rate_limit])
# ... etc
```

`auth`/`rate_limit` are ordinary callables resolved at compile time (callable
references, spec §`&target`), so the middleware list is a typed array of
functions, not a string-keyed lookup.

## 5. Middleware

A middleware is just a typed function `(Request, Next) -> Response`; `before:`
attaches it. No special class hierarchy:

```amber
def auth(req, next):
  case req.header("authorization"):
    Some(tok) and valid?(tok): next(req)
    _: Response.unauthorized()
```

The macro composes the `before:` list around the handler at expansion time into
a straight-line call chain (`auth(req) { rate_limit(req) { handler(req) } }`),
so the composition is visible to the type checker and inlinable — not a runtime
fold over a list of unknown callables.

## 6. Derived serialization (`@derive`)

```amber
@derive(Json)
class User:
  id:    Int
  name:  Str
  email: Str
```

`@derive(Json)` is a derive macro (macro doc §8.3): it reads the class's
declared fields and emits a typed `to_json` / `from_json` over the existing
`Json` stdlib (`DESIGN-stdlib-json-api-2026-06-16.md`) — the serde pattern,
fully typed, no reflection at runtime:

```amber
class_method def from_json(j: Json.Value) -> Result[User, Json.Error]: ...
def to_json(self) -> Json.Value:
  Json.object({ "id": self.id, "name": self.name, "email": self.email })
```

## 7. Controllers via `use` (injection macro)

```amber
class PostsController:
  use ember.Controller        # injection macro (macro doc §8.4)

  @route(GET, "/posts/:id")
  def show(req, id: Int):
    render(:post, post: Posts.find(id))   # `render` injected by `use`
```

`use ember.Controller` is an injection macro: at compile time it emits the
controller's shared members (`render`, `redirect`, `params`, the typed response
helpers) directly into the class body, so `render` is a real method resolved
statically — not a `method_missing` fallback. This is the Phoenix `use`
ergonomic without the runtime indirection.

## 8. Request / Response surface (runtime, not macro)

These are ordinary runtime types over `net.http`; macros only *generate calls
into* them.

```text
Request:  .method  .path  .path_param(name)  .query(name)  .header(name)
          .body_text  .body_json  .params  (strand-confined)
Response: ok(body)  json(value)  not_found()  bad_request(msg)
          unauthorized()  redirect(url)  status(code).body(...)  (immutable)
App:      ember.App(name:)  .routes: <block>  .serve(listen_cap)
```

`.serve(listen)` requires a `net.listen` capability (the server-side analogue of
the existing capability-gated `net.connect` used by `net.http.Client`); without
it, construction fails before any socket is opened (the framework inherits the
`net.http` capability-denial behavior).

## 9. Why this shape (vs the alternatives)

| Concern            | Ember (macros)                  | `method_missing` router        |
|--------------------|---------------------------------|--------------------------------|
| Route dispatch     | built table, typed lookup       | reflective, `Any`-boundary     |
| Path params        | generated typed parse + 400     | runtime cast in handler        |
| Middleware         | composed call chain, inlinable  | runtime fold over `Any` list   |
| Serialization      | derived typed `to_json`         | runtime field reflection       |
| Errors in handlers | checked at compile time         | surface at runtime             |
| Cost               | zero beyond the generated code  | reflective slow path per req   |

The DSL reads like Rails/Sinatra/Phoenix, but every construct resolves to
ordinary typed Amber before the binder runs, so it is checked by the typed
profile and devirtualized by the VM. That is the entire argument for adding
macros (macro doc §1): a web DSL is exactly the case where compile-time codegen
beats the runtime MOP on both safety and speed.

## 10. Open ends (sketch-level)

- **Path matching**: trie vs compiled regex for the built route table; `:id`
  vs `*splat` vs typed segment (`/users/:id<Int>`) — the macro could push the
  type into the path literal instead of the param list.
- **Streaming responses**: ties into the `net.http` Phase 3 streaming gap
  (`ResponseBody`/`io.Writer`) still open in the runtime.
- **Content negotiation, sessions, CSRF, templating**: out of scope for the
  first approximation; templating is itself a natural second macro DSL.
- **Hot reload**: explicitly out (Q3); the framework is boot-then-freeze, which
  suits the macro + frozen-world model well.
- **Name**: "Ember" is a placeholder.
