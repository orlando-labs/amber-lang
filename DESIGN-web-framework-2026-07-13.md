# DESIGN: Ember — единый web framework для Amber

Date: 2026-07-13  
Status: consolidated design proposal; no implementation in this document  
Target: external Amber package `ember` over the existing `net.http.Server`

This document supersedes the design direction of:

- `DESIGN-web-framework-dsl-sketch-2026-06-29.md`;
- `DESIGN-web-framework-dsl-rails-variant-2026-06-29.md`.

Those files remain useful as historical sketches, but this document is the
single source of truth for the proposed framework shape. In particular, it
removes handler-local route annotations, unifies the minimal and MVC variants,
uses the real Amber callable-reference semantics, and accounts for the current
`amber-orm` and `sqlite3-amber` concurrency constraints.

---

## 0. Executive summary

Ember is one framework with one router and two ordinary ways to organize an
application:

1. A small application binds routes directly to module functions.
2. An MVC-shaped application binds the same routes to controller instance
   methods and keeps the root route declaration in `config/routes.am`.

There is no separate "light" and "heavy" runtime. Controllers, views,
resources, ORM integration, generators, and directory conventions are layers
over the same `App`, `Routes`, `Request`, `Response`, and middleware contracts.

The canonical route declaration is:

```amber
routes = ember.routes:
  get "/", to: &handler
```

`ember.routes` is the only route-table constructor name. It is a block-suffix
macro that returns an immutable `ember.Routes` value. It does not mutate a
package-global registry and does not attach metadata to handler declarations.

Controller actions use Amber's unbound instance-method reference syntax:

```amber
routes = ember.routes:
  get "/", to: &Home#landing

class Home < ember.Controller:
  def landing():
    render text: "o hai"
```

`&Home.landing` is reserved for a real class-side method declared with
`class_method def`; it does not refer to the ordinary instance method above.

A URL backed by a mutable database entity does not require a mutable route
table. The router keeps a static shape such as `/people/:slug`; the captured
slug is resolved through a repository at request time:

```amber
routes = ember.routes:
  get "/people/:slug", to: &People#show, as: :person
```

Changing an entity's slug changes database data, not router structure. Slug
history can redirect old URLs to the new canonical URL. A CMS that owns
arbitrary paths uses one lowest-precedence catch-all route such as `/*path` and
resolves the full normalized path through the database.

---

## 1. Grounded substrate

This design builds on implementation that already exists in the Amber
workspace:

- block-suffix, `use`, annotation, and string-tag macro surfaces exist under
  `macro.v1`;
- callable references support module functions, class-side sends, and unbound
  instance methods;
- `net.http.Server#serve` accepts a request hook and returns a
  `ServerResponse`-compatible value;
- the server runs request hooks cooperatively and may have multiple concurrent
  requests;
- `amber-orm` implements its P0 model, validation, query, and adapter surfaces,
  plus a thin SQLite pool facade, one-time model binding, and model-level
  transactions;
- `sqlite3-amber` implements database access, safe parameterized SQL, and a
  shareable connection pool with exclusive leases, cooperative waiting, and a
  non-inheritable logical-task-local current-connection context.

Ember is therefore primarily an Amber package plus macros. It should not add a
second HTTP scheduler, runtime metaprogramming layer, or socket implementation.

The low-level server currently remains intentionally small: plaintext HTTP/1.1,
one request per accepted connection, `Connection: close`, and buffered server
request/response bodies. Ember v1 must describe these limitations honestly and
must not imply TLS, HTTP/2, keep-alive, WebSocket, or streaming support that the
substrate does not provide.

---

## 2. Design anchors

### 2.1. Central routes, no route annotations

Routes are application topology and belong in an explicit route declaration.
They are not attributes of handler definitions.

Rejected:

```amber
route GET, "/posts/:id"
def show(request, id):
  # ...
```

Canonical:

```amber
routes = ember.routes:
  get "/posts/:id", to: &show
```

This gives one searchable map of HTTP methods, paths, targets, middleware,
names, and constraints. A large application may compose explicitly named
subrouters, but one root routes module remains the entry point.

This rule rejects annotations that declare HTTP method/path/name or register a
route. It does not reject a function-intrinsic execution-policy annotation such
as `before database_request`: that annotation wraps every invocation of the
function and does not mutate `Routes`. Route-specific middleware remains in the
central route declaration.

### 2.2. Macros generate ordinary Amber

`ember.routes` and its nested DSL forms consume AST during F1.5 and generate
ordinary declarations, route descriptors, target wrappers, and constructors.
The binder, typed checker, HIR, and runtime see ordinary Amber after expansion.

Macros are not used for request-time dispatch. Runtime routing operates over a
prebuilt immutable matcher.

### 2.3. No runtime `load`

`config/routes.am` is an ordinary Amber module. It is included in the build
manifest and imported statically. Ember does not reproduce Ruby's `load`,
caller-scope evaluation, or repeated source execution.

Convention may decide which module an application generator imports, but the
semantic edge remains a normal Amber import.

### 2.4. No ambient current request

The framework does not maintain a global or task-id-indexed "current request".
Function handlers receive a request explicitly. Controller instances hold
request-scoped state explicitly.

This preserves strand isolation and avoids cancellation leaks, accidental
child-task inheritance, and hidden capability access.

### 2.5. Immutable shared topology, request-scoped mutable state

The following may be shared across request strands:

- frozen application configuration;
- immutable `Routes` and its matcher;
- immutable middleware configuration;
- shareable service factories and future database pools.

The following are request-scoped:

- normalized `Request`;
- path captures and parsed params;
- controller instance;
- response builder;
- session/flash state;
- database connection lease and repositories.

---

## 3. Package shape

One repository may expose several modules without creating separate
frameworks:

```text
src/
  ember.am                  App, Request, Response, render
  ember/router.am           Routes, matcher, route macros
  ember/controller.am       Controller and `use ember.controller`
  ember/middleware.am       middleware composition
  ember/html.am             Html value and html string-tag macro
  ember/testing.am          in-memory request helpers
  ember/session.am          later phase
  ember/security.am         later cookies/CSRF/security headers

integrations/
  orm.am                    optional ORM integration
```

The minimal import must not force SQLite, FFI, ORM, templates, sessions, or a
project generator into the application. An application that imports only
`ember` gets the same router and HTTP adapter as an MVC application, with fewer
layers configured.

ORM glue may live in an optional module or a small companion package so that
the core web package remains source-only and database-neutral.

---

## 4. `ember.routes`

### 4.1. Canonical expression form

`ember.routes` is a block-suffix macro returning `ember.Routes`:

```amber
routes = ember.routes:
  get "/", to: &home
  post "/sessions", to: &create_session
```

Assignment is explicit because applications and tests may construct more than
one router. A bare unused `ember.routes:` statement should be diagnosed as
`EMBER_ROUTES_UNUSED` rather than silently creating a hidden global registry.

A conventional routes module is:

```amber
package app.routes

import ember
from app.controllers.home import Home
from app.controllers.posts import Posts

export routes

routes = ember.routes:
  get "/", to: &Home#landing, as: :root
  resources :posts, controller: Posts
```

The application entry imports it normally:

```amber
from app.routes import routes
import ember

app = ember.App(routes: routes)
```

### 4.2. Return value and ownership

`ember.routes` returns one immutable object containing:

- normalized route descriptors;
- a path matcher;
- method tables;
- route names and reverse-path metadata;
- generated target wrappers;
- route-specific middleware chains;
- source metadata for diagnostics and introspection.

There is no mutable `ember.__routes` singleton. `App` receives routes through
its constructor:

```amber
app = ember.App(routes: routes)
```

### 4.3. Basic DSL

The first useful surface is intentionally small:

```amber
routes = ember.routes:
  get     "/items",     to: &items_index, as: :items
  get     "/items/:id", to: &Items#show,  as: :item
  post    "/items",     to: &Items#create
  put     "/items/:id", to: &Items#replace
  patch   "/items/:id", to: &Items#update
  delete  "/items/:id", to: &Items#destroy
  options "/items",     to: &items_options
```

`head` may be declared explicitly. Whether GET supplies an automatic HEAD
route is an application option and must not be left implicit in the matcher.

### 4.4. Scope, middleware, and composition

```amber
routes = ember.routes:
  scope "/api", before: [&request_id, &json_errors]:
    scope "/v1", before: [&authenticate]:
      get "/profile", to: &Profile#show

  mount AdminRoutes, at: "/admin"
```

Prefixes and statically known middleware lists are flattened by the macro.
`mount` is explicit composition: it does not scan directories or discover
controller annotations.

The root route file remains the readable index even when implementation is
split across subrouters.

### 4.5. `resources` is sugar, not a second router

The MVC layer may provide:

```amber
routes = ember.routes:
  resources(:posts, controller: Posts,
    only: [:index, :show, :create, :update, :destroy])
```

The macro expands this to ordinary route descriptors and correct unbound
instance method references. No strings such as `"posts#show"` participate in
dispatch.

`resources` belongs after the primitive verbs, scopes, params, names, and
reverse paths are stable. It must not define different request or response
semantics.

---

## 5. Route targets and controller lifecycle

### 5.1. Callable-reference semantics

Amber distinguishes:

```amber
&home                 # module/top-level function
&Home.landing         # class-side method
&Home#landing         # unbound instance method
```

Therefore the canonical controller target is:

```amber
get "/", to: &Home#landing
```

For this class:

```amber
class Home < ember.Controller:
  def landing():
    # ...
```

The dot form is valid only when the action is actually class-side:

```amber
class Health:
  class_method def show(request):
    ember.render text: "ok"

routes = ember.routes:
  get "/health", to: &Health.show
```

Ember does not reinterpret `&Class.method` as an instance action. Doing so
would contradict core Amber syntax and make ordinary callable references
framework-dependent.

### 5.2. Target normalization

The macro generates a common internal handler wrapper for each route:

| Public target | User declaration | Generated conceptual call |
|---|---|---|
| `&handler` | `def handler(request)` | `handler(request)` |
| `&Home#landing` | `def landing()` | `Home(request).landing` |
| `&Health.show` | `class_method def show(request)` | `Health.show(request)` |
| function with path params | `def show(request, id as Int)` | `show(request, id)` |
| controller with path params | `def show(id as Int)` | `Home(request).show(id)` |

Each generated wrapper ultimately has the same framework contract:

```text
(ember.Request) -> ember.Response
```

This permits one route table without reflective method names or a heterogeneous
`Any` handler array.

Optional integrations may decorate the generated wrapper while preserving the
same direct, statically visible target call. In particular, the ORM integration
may establish a connection lease around controller construction and action
execution without changing the controller action signature. Bare function
targets are not given that controller scope unless middleware or an annotation
explicitly requests it.

### 5.3. Controller construction

A controller is instantiated once per request dispatch:

```amber
controller = Home(request)
result = controller.landing
```

`ember.Controller` owns:

- the request;
- access to the application's routes and reverse-path helpers;
- path/query/body param access;
- response status and header builder;
- render/redirect helpers;
- request-scoped services;
- later session/flash helpers.

Controller instances are never pooled or shared.

The canonical composition form is inheritance:

```amber
class Home < ember.Controller:
```

An alternative injection macro may be supplied for a class that already needs
another superclass:

```amber
class Home < ApplicationObject:
  use ember.controller
```

The macro is lower-case to distinguish it from the runtime `Controller` class,
following the existing `use orm.model` style.

---

## 6. Request and response surface

### 6.1. Request

`ember.Request` is the normalized, request-scoped value passed through routing
and middleware. It exposes at least:

```text
method, target, path, raw_query
headers, body_text, body_bytes
path_param(name), query(name)
remote_endpoint, local_endpoint
routes
service(name)
```

`routes` is the `Routes` instance owned by the current `App`; it permits
reverse-path generation without importing the routes module from a handler.
`service(name)` resolves only from the explicit application/request service
scope. Neither property is an ambient global.

The request is logically immutable after matching. Adding decoded captures,
the selected route, and the request service scope produces the routed request
view used by middleware and the target wrapper; handlers do not mutate the
low-level `ServerRequest`.

### 6.2. Minimal function handler

```amber
import ember

def handler(request):
  ember.render text: "o hai", status: :ok
```

`ember.render` is a pure response constructor in a module function. It does not
read a hidden current request.

The following free-function spelling is deliberately not supported:

```amber
def handler(request):
  status :ok
  render text: "o hai"
```

Two independent module-function calls cannot share response state without an
ambient current exchange or a macro that rewrites the handler declaration.
Both mechanisms conflict with the central-routing and explicit-state design.

### 6.3. Controller response builder

Controller methods may use receiver-local response state:

```amber
class Home < ember.Controller:
  def landing():
    status :ok
    header "cache-control", "no-store"
    render text: "o hai"
```

`render` returns a completed `ember.Response`. It does not perform an implicit
language-level return. An early response is explicit:

```amber
return render text: "invalid", status: :unprocessable_entity
```

A second render after the response has been completed raises
`DoubleRenderError`. Mutating status or headers after completion raises
`ResponseCommittedError`.

### 6.4. Response normalization

The framework boundary accepts a deliberately small response-like set:

- `ember.Response` — unchanged;
- `Str` — status 200, `text/plain; charset=utf-8`;
- `Bytes` — status 200, `application/octet-stream`;
- `Html` — status 200, `text/html; charset=utf-8`;
- `null` — status 204;
- anything else — `InvalidResponseError`.

Maps and arbitrary model objects are not implicitly serialized as JSON. JSON
must be explicit:

```amber
ember.render json: {message: "ok"}, status: :ok
```

This prevents accidental serialization of model fields, secrets, ORM dirty
state, or framework objects.

### 6.5. Status symbols

Ember maps known symbols to numeric HTTP statuses before constructing the
low-level response:

```text
:ok                     200
:created                201
:accepted               202
:no_content             204
:moved_permanently      301
:temporary_redirect     307
:permanent_redirect     308
:bad_request            400
:unauthorized           401
:forbidden              403
:not_found              404
:method_not_allowed     405
:gone                   410
:unprocessable_entity   422
:internal_server_error  500
```

Unknown symbols are errors. Numeric statuses remain available for extension
codes subject to the low-level HTTP validity range.

---

## 7. Route matching

### 7.1. Matcher model

The recommended matcher is a segment trie or an equivalent immutable compiled
structure with:

- literal edges;
- parameter edges;
- one terminal wildcard edge;
- a method table at matched leaves.

The lookup result is conceptually:

```text
route_id + raw captures + allowed methods
```

The generated target wrapper parses typed captures and invokes the handler.

### 7.2. Precedence

Matching precedence is structural and independent of declaration order:

```text
literal > parameter > wildcard
```

Consequences:

```amber
get "/people/new",   to: &People#new_
get "/people/:slug", to: &People#show
```

`/people/new` always selects the literal route even if the parameter route is
written first.

Routes with the same normalized shape are ambiguous and fail at build time:

```amber
get "/people/:id",   to: &People#by_id
get "/people/:slug", to: &People#by_slug
```

Parameter names do not make matcher shapes different.

Parameter conversion types do not make matcher shapes different either:

```amber
get "/people/:value", to: &People#by_id, params: {value: Int}
get "/people/:slug",  to: &People#by_slug
```

These routes are ambiguous. `params: {value: Int}` converts a capture after a
route has matched; it is not a fallback routing predicate. A later explicit
`constraints:` feature may support value-dependent dispatch, but conversion
failure in the basic design produces a bad-request response rather than trying
another route.

### 7.3. Required path policy

Before implementation, Ember must fix:

- strict, ignored, or redirected trailing slash behavior;
- HEAD fallback to GET;
- automatic OPTIONS policy;
- 404 versus 405 behavior;
- wildcard syntax and empty-wildcard semantics;
- percent-decoding and invalid encoding errors;
- maximum path, segment, capture, and route counts;
- repeated parameter-name diagnostics;
- host/scheme constraints if they enter v1.

Path segmentation must occur before percent-decoding so encoded `/` cannot
silently change route structure. Query strings never participate in path
matching.

---

## 8. Typed route params

Central routes cannot infer parameter types by reading an annotated handler AST
from another module. The type must therefore be part of the route declaration
or remain explicit handler parsing.

Recommended explicit form:

```amber
routes = ember.routes:
  get "/people/:id",
    to: &People#show,
    params: {id: Int},
    as: :person
```

```amber
class People < ember.Controller:
  def show(id as Int):
    # `id` is already Int
```

The macro generates parse-or-400 code and a statically visible call to
`People#show`. Under the typed profile, the checker validates the generated
call against the action signature.

An untyped string parameter is the default:

```amber
get "/people/:slug", to: &People#show
```

The handler sees `slug as Str` or uses `request.path_param(:slug)` in dynamic
code.

The route macro can always validate literal route grammar, duplicate shapes,
unknown parameter declarations, and target-reference form. Full signature and
return-type guarantees require the typed profile; the dynamic profile retains
ordinary Amber runtime errors where static proof is unavailable.

---

## 9. Dynamic entity URLs and slugs

### 9.1. Dynamic URL is not dynamic topology

When a database entity has a mutable human-readable URL, the route shape is
normally static and only the capture value is dynamic:

```amber
routes = ember.routes:
  get "/people/:slug", to: &People#show, as: :person
```

The database may contain:

```text
people.id   people.slug
1           ada-lovelace
2           grace-hopper
```

The router does not contain one route per person. It contains one route whose
wrapper asks the repository to resolve the slug.

Conceptual action:

```amber
class People < ember.Controller:
  def show(slug as Str):
    repo = service(:people)
    case repo.find_by_slug(slug):
      Ok(person):
        render Views.people.show(person: person)
      Err(NotFound):
        render text: "not found", status: :not_found
```

This keeps the route table immutable and shareable while entities may be
created, renamed, or deleted without rebuilding application topology.

### 9.2. Canonical slug changes

Changing a slug must be a database transaction, not a router mutation. A
minimal data model stores the current slug on the entity and previous paths in
a history table:

```text
people
  id
  slug
  ...

entity_paths
  tenant_id             optional scope
  locale                optional scope
  normalized_path
  entity_kind
  entity_id
  canonical             Bool
  redirect_status
  created_at

  unique(tenant_id, locale, normalized_path)
```

For a resource-scoped URL, the history table may store only `(scope, slug)`;
for CMS paths it stores the complete normalized path.

Changing `ada-lovelace` to `ada-king` should atomically:

1. normalize and validate the new slug;
2. check reserved names;
3. claim the new unique path;
4. update the entity's canonical slug;
5. mark the old path as historical;
6. commit;
7. invalidate a dynamic-path cache after commit, if one exists.

A request to the historical path resolves the entity and redirects to the
current named route:

```amber
redirect to: path(:person, slug: person.slug),
  status: :permanent_redirect
```

`path` here is an inherited controller helper that delegates to the routes
stored in the request/application context. The controller does not import
`app.routes`: that would create a module cycle because `app.routes` already
imports the controller.

Defaulting to 308 preserves the HTTP method. An application focused on
GET/HEAD SEO may explicitly choose 301. Redirect chains must be collapsed to
the current canonical URL, and path updates must not create cycles.

### 9.3. Root-level human-readable slugs

An application may intentionally place entity slugs at the root:

```amber
routes = ember.routes:
  get "/login", to: &Sessions#new_
  get "/about", to: &Static#about
  get "/:slug", to: &People#show, as: :person
```

Literal precedence protects `/login` and `/about`, but the application still
needs a reserved-slug policy. Otherwise a person may successfully claim
`login`, yet their page will never be reachable because the literal route wins.

The reserved set should include:

- the root router's literal first segments;
- mounted prefixes such as `assets`, `api`, and `admin`;
- application-defined future reservations;
- locale or tenant prefixes where applicable.

The database unique constraint, not an in-memory preflight, is the final
authority under concurrent edits.

### 9.4. Arbitrary CMS paths

A CMS may allow `/about`, `/docs/install`, and `/products/widget` to refer to
different database rows. Registering each path as a mutable route is still
unnecessary. Use one lowest-precedence wildcard resolver:

```amber
routes = ember.routes:
  get "/assets/*path", to: &Assets#show
  get "/api/*path",    to: &Api#dispatch

  get "/*path", to: &Content#resolve, as: :content
```

The wildcard capture is normalized and looked up in a `content_paths` table.
Static and parameter routes win before the wildcard. The resolver returns:

```text
Canonical(content_id)
Redirect(content_id, canonical_path, status)
Gone
NotFound
```

This makes database ownership explicit and allows logging, caching, preview
permissions, publication state, locales, and tenant boundaries to live in the
content resolver rather than the core matcher.

### 9.5. Normalization policy

Slug/path normalization must be one shared library used on both write and read
paths. It must define at least:

- Unicode normalization form;
- case sensitivity or folding;
- percent-decoding rules;
- repeated and trailing slash behavior;
- leading slash storage;
- empty segments;
- maximum bytes and segment count;
- whether `.` and `..` are rejected;
- locale and tenant scoping;
- query and fragment exclusion.

Display text is not necessarily the lookup key. An application may keep the
original Unicode slug for rendering and store a normalized unique key for
matching.

Slug generation itself is application policy. Ember may later provide helpers
for transliteration and suffixing, but the router must not silently generate or
mutate slugs.

### 9.6. Cache behavior

A dynamic resolver may cache normalized path resolutions, including bounded
negative entries, but:

- the database remains the source of truth;
- cache entries are immutable values;
- cache keys include tenant and locale scope;
- updates invalidate only after transaction commit;
- TTL bounds stale redirects and deletions;
- a cache failure falls back to repository lookup;
- cache contents never mutate `Routes`.

### 9.7. Truly dynamic route topology

Database-controlled HTTP methods, middleware chains, handler names, or route
patterns are a separate feature from slugs. They raise code-loading,
authorization, validation, atomic publication, and cache invalidation problems.

If required later, the safe model is:

1. read declarative route data;
2. validate it against an allowlist of precompiled handlers and middleware;
3. compile a new immutable matcher snapshot;
4. atomically swap the snapshot for new requests;
5. let in-flight requests finish on the previous snapshot.

This is out of Ember v1. Arbitrary handler names from a database must never be
sent through `send` or `method_missing` on the request hot path.

---

## 10. Named routes and reverse paths

Named routes provide one canonical way to create URLs:

```amber
routes = ember.routes:
  get "/people/:slug", to: &People#show, as: :person
```

```amber
routes.path(:person, slug: person.slug)
# => "/people/ada-lovelace"
```

Inside a controller the same operation is available without importing the
routes module:

```amber
path(:person, slug: person.slug)
```

A module-function handler uses the explicit request context:

```amber
request.routes.path(:person, slug: person.slug)
```

`path` validates:

- that the route name exists;
- that every required path parameter is present;
- that no unknown path parameter is supplied;
- that values satisfy path encoding and length policy.

`url` additionally applies an explicit or configured origin:

```amber
routes.url(:person, slug: person.slug, origin: request.origin)
```

Database rows should store canonical identifiers/slugs, not pre-rendered full
URLs. This keeps host, mount prefix, locale, and deployment origin out of model
data.

Generated statically typed helper methods may be added later, but the first
implementation may use `routes.path(:name, **params)` with deterministic
runtime validation.

---

## 11. Middleware and errors

Middleware has one ordinary callable contract:

```text
(Request, Next) -> Response
```

Example:

```amber
def authenticate(request, next):
  if request.user:
    next(request)
  else:
    ember.render text: "unauthorized", status: :unauthorized
```

Middleware layers are ordered from outermost to innermost:

1. application middleware;
2. mounted/scope middleware;
3. route middleware;
4. target wrapper.

The framework should ship narrow default middleware for:

- exception-to-500 conversion with production-safe output;
- request ids;
- access logging;
- method/path normalization errors;
- optional body-size/content-type parsing.

Authentication, authorization, transactions, sessions, CSRF, and domain error
mapping remain explicit application configuration.

Expected failures should use ordinary `Result` values and exhaustive handling.
Exceptions are reserved for unexpected failures or APIs whose established
contract raises.

### 11.1. Function-intrinsic `before` annotation

Middleware that is an invariant of a module function, rather than policy of one
particular route, may be attached with Amber's annotation macro surface:

```amber
from ember.middleware import before
import ember.integrations.orm as ember_orm

database_request = ember_orm.connection(database)

before database_request
def create(request):
  Person.create(name: request.query(:name))
```

`before` is an annotation-shaped `macro def`. The macro receives its middleware
syntax and the annotated declaration `Ast`, then returns one replacement
declaration whose body invokes the middleware around the original body:

```amber
macro def before(middleware as Ast, declaration as Ast) -> Ast:
  # Validate AstDefStmt, preserve the declaration signature, wrap its body.
  ...
```

The middleware expression is not evaluated by the macro. It is spliced into
the generated runtime wrapper and resolved by the ordinary binder. The public
function name, typed/effect signature, source mapping, and callable reference
such as `&create` remain unchanged.

P0 accepts one middleware or one ordered list:

```amber
before [&request_id, &authenticate, database_request]
def create(request):
  ...
```

The list is applied outermost-to-innermost in written order. One list is
preferred to stacked `before` annotations because general annotation expansion
composes top-to-bottom while wrapper nesting can otherwise read in the reverse
runtime order. P0 supports ordinary module `AstDefStmt` declarations; clause
families need a separate deterministic expansion rule.

This is not a handler-local route declaration:

- it does not declare method, path, route name, params, or route registration;
- it applies to every direct invocation of the function, including tests;
- policy that differs between routes stays in `routes` via `before:`;
- a blank line between the annotation and `def` breaks attachment by the normal
  Amber annotation rule.

---

## 12. Views

### 12.1. Typed view calls

Controllers render values, not string paths:

```amber
render Views.posts.index(posts: posts)
```

Rejected:

```amber
render "posts/index"
render :index
```

A missing view binding, missing argument, or incorrect argument type should be
reported by ordinary binding/type checking.

### 12.2. First implementation: Amber modules

The first view implementation can use ordinary `.am` modules and an inline
HTML string-tag macro:

```amber
package app.views.posts

from ember.html import html

export index

def index(posts as List[Post]):
  html"""
    <h1>Posts</h1>
    <ul>
    %for post in posts:
      <li>#{post.title}</li>
    </ul>
  """
```

`Html` is a distinct safe value. Text and attribute interpolation escape by
default. Raw output requires an explicit audited type/helper.

### 12.3. External `.amv` files

External view files require compiler/build support, not filesystem access from
an Ember macro. The build layer must:

- declare external files as build inputs;
- map suffixes to exported string-tag macro providers;
- construct `Ast.StringTemplate` values with external source spans;
- generate virtual Amber modules under a mounted namespace;
- include provider and input digests in build cache keys;
- report parser, binder, and type errors against the external file.

This should become a general Amber source-format registry usable by HTML, SQL,
GraphQL, protobuf, and other packages. It should not be hard-coded into the
web framework.

The exact registry/manifest syntax remains a separate compiler-level design.
Until then Ember documentation must not present `.amv` mounting as an already
implementable package-only macro.

SQL template providers may safely parameterize interpolations, but declaring a
typed result such as `List[Post]` additionally requires schema/result-shape
validation that `sqlite3-amber` does not currently provide.

---

## 13. ORM and database integration

### 13.1. Core Ember is ORM-neutral

Ember's router, request, response, middleware, and controller contracts do not
depend on `amber-orm`. A handler may use raw SQLite, a repository, an HTTP
service, an in-memory store, or no persistence.

### 13.2. One-time model binding to a shareable pool

Binding a shared model to a confined physical `Database` remains unsafe:

```amber
User.bind!(orm_sqlite.adapter(confined_database))
```

The web shape binds models once, before concurrent serving, to an ORM facade
over the shareable low-level pool:

```amber
database = orm_sqlite.pool("data/app.db", max_size: 8)
database.bind!(Person, EntityPath).or_raise
```

The model config stores a shareable pool-backed executor, never a request's
physical connection. Per-request rebinding is forbidden. Calls reuse an
already checked-out connection in the current logical task; outside an explicit
scope the low-level Pool can checkout/release around one isolated operation.

The implemented package is split deliberately: `orm.sqlite3` contains only
public constructors, `orm.sqlite3.adapter` contains the SQLite executor,
`orm.sqlite3.pool` contains the wrapper, and `orm.pool` contains DB-neutral
connection/binding errors. The web integration depends on those public
surfaces; it does not add a second pool implementation.

### 13.3. Controller profile

The optional ORM integration owns the controller execution scope:

```amber
app = ember.App(
  routes: routes,
  integrations: [ember_orm.pool(database)]
)
```

Conceptually it decorates a controller target as:

```amber
database.with_connection:
  controller = People(request)
  result = controller.show(id)
  normalize(result)
```

The action remains ordinary model code:

```amber
class People < ember.Controller:
  def show(id as Int):
    match Person.find(id):
      when Ok(person):
        render Views.people.show(person: person)
      when Err(error):
        render text: "not found", status: :not_found
```

There is no pool/session/repository argument in the action. The connection
scope covers controller construction, action execution, and buffered response
or view materialization, then releases in `ensure`. It does not start a
transaction automatically.

### 13.4. Bare function profile

A bare `&handler` has no controller lifecycle and receives no hidden ORM scope.
It chooses one explicit form:

1. route middleware: `before: [database_request]`;
2. function-intrinsic annotation: `before database_request` directly above the
   `def`;
3. `request.service(:database).with_connection` in the handler.

After one-time model binding an isolated `Person.find(id)` can technically use
operation-local checkout, but that is not a request scope: sequential calls may
use different physical connections. Multi-operation handlers use middleware,
the annotation, an explicit connection scope, or a transaction.

This mirrors the broader framework distinction. Controller receiver state
supports `status`, `header`, and controller integration; a free function has
only its explicit arguments, annotations, and configured middleware.

### 13.5. Pool failures and shutdown boundary

The ORM facade raises DB-neutral `orm.pool.ConnectionPoolTimeoutError`,
`ConnectionPoolClosedError`, and `ConnectionPoolLeaseError` at explicit scope
boundaries. Rebinding a model to a different pool returns
`Err(ModelBindingError)` during application boot.

For a controller target, checkout happens before controller construction. The
optional integration maps an unavailable/timeout failure to a production-safe
503 response and logs the structured source error; it must not invoke the
action or expose the native SQLite message. A bare handler may rescue the same
error explicitly, but route middleware normally owns that boundary.

Application shutdown stops accepting work, waits for in-flight target scopes,
then calls `database.drain!`/`close!`. Request/controller code does not own pool
lifecycle.

### 13.6. SQLite concurrency and transaction boundary

`sqlite3-amber` now reports `pool: true`, `pool_affinity: :lease`, and
`pool_context: :task_local`. Its non-inheritable task-local binding survives
suspension and worker migration; a `task.spawn` child obtains a separate lease.
Open statements are finalized and unfinished transactions rolled back on
release.

The ORM integration therefore delegates pooling rather than implementing a
second queue or task-local registry. Pool size, WAL, busy timeout, checkout
timeout, and retry policy remain explicit deployment choices. SQLite writers
remain serialized by SQLite locking semantics even when the pool has multiple
connections.

Transactions are explicit business boundaries, for example
`Person.transaction(mode: :immediate): ...`; controller connection scope alone
does not imply a transaction.

Schema-derived compile-time strong params remain possible only when the schema
is present in source or a declared snapshot. Runtime-inferred SQLite schemas
cannot give the compiler field information retroactively.

---

## 14. Application and server boundary

`App` is a callable request pipeline independent of socket ownership:

```amber
app = ember.App(
  routes: routes,
  middleware: [&request_id, &access_log],
  integrations: [ember_orm.pool(database)]
)

response = app.call(request)
```

The HTTP adapter is thin:

```amber
server.serve |server_request|:
  app.call(ember.Request.from_server(server_request)).to_server_response
```

Convenience may construct the current low-level server:

```amber
app.run(
  host: "127.0.0.1",
  port: 3000,
  workers: 2,
  max_concurrent_per_worker: 32
)
```

But `App#call` is the primary semantic boundary. Tests, alternate servers, a
future TLS transport, or an in-process caller use the same pipeline.

Network/listen capabilities remain enforced by `net`/`net.http`; Ember does not
manufacture or hide capabilities.

`integrations:` is optional application composition. An integration may
decorate typed target wrappers, add explicit request services, and participate
in shutdown, but it does not mutate `Routes` or create process-global current
state. Core Ember remains usable without ORM imports.

### 14.1. Bidirectional cooperative streaming

Streaming support spans the low-level HTTP transport and Ember. It is a
protocol feature, not a buffered-response convenience API. The first
implementation covers HTTP/1.1 response chunked transfer coding and streaming
request consumption for both chunked and Content-Length framing. Chunk
extensions and trailer fields follow RFC 9112.

Streaming must be cooperative from its first implementation. Waiting for
socket readability, socket writability, producer data, or downstream capacity
parks only the current logical task. It must not block an OS worker and must
allow other requests and streams owned by the same scheduler worker to make
progress.

#### 14.1.1. Streaming response selection and commitment

Selecting a streaming body is the response commitment point. A controller
shape may be:

```amber
class Events < ember.Controller:
  def index():
    stream(
      status: :ok,
      headers: {"content-type": "application/x-ndjson"},
      trailers: ["digest"]
    ) |body|:
      body.write("{\"event\":1}\n", extensions: {sequence: "1"})
      body.write("{\"event\":2}\n", extensions: {sequence: "2"})
      body.close
      body.trailer("digest", "sha-256=:...:")
```

`stream` returns a `StreamingResponse` whose producer is executed by the
adapter. As soon as this response is selected, before the producer performs its
first write:

- the status is fixed;
- the initial header map is fixed;
- the controller response builder is committed;
- later `status`, `header`, `render`, `redirect`, or second `stream` calls raise
  `ResponseCommittedError` or `DoubleRenderError` as appropriate.

The commitment rule is based on selecting the streaming body, not on whether
the adapter has already flushed header bytes to the socket. This keeps behavior
deterministic in tests and prevents a producer race from changing metadata.

For an HTTP/1.1 streaming response Ember automatically adds
`Transfer-Encoding: chunked` before freezing the headers. `Content-Length` is
forbidden, and an application-supplied `Transfer-Encoding` must not override or
extend the framework framing. Declared trailer names automatically produce the
corresponding `Trailer` header. Streaming bodies are rejected for response
statuses and request methods whose HTTP semantics do not permit a body; HEAD
uses the same selected metadata but never runs a body producer.

#### 14.1.2. Response chunk contract

Every successful non-empty `body.write(data, extensions: ...)` call is exactly
one semantic HTTP chunk:

```text
chunk-size [chunk extensions] CRLF
chunk-data CRLF
```

Ember and the low-level encoder must not merge adjacent writes into one chunk
or split one write into several semantic chunks. The TCP layer may of course
split the encoded bytes across packets. Empty writes are rejected or specified
as a no-op; they must never encode a zero-size data chunk because zero denotes
the end of a chunked body.

`write` accepts `Bytes` and explicitly encoded `Str` values. It completes only
after the chunk has entered the bounded transport queue or has been written.
If that queue is full, the current producer task parks until capacity becomes
available. This is the response-side backpressure boundary.

Chunk extensions use an ordered immutable representation because order and
repeated names must not be accidentally lost. A map is accepted only as a
convenience for unique names. Each extension has a token name and an optional
token or quoted-string value. The encoder validates and escapes values; raw
extension text, CR, LF, and other framing injection are never accepted.

#### 14.1.3. Response close and trailers

The response stream has explicit states:

```text
open -> body_closed -> finished
         |                |
         +-- trailers ----+
```

`body.close` closes the data-chunk phase and writes the terminating zero-size
chunk marker, but keeps the trailer section open. No `write` is valid after
that transition. Declared trailer fields may then be sent with
`body.trailer(name, value)`. Finishing the producer closes the trailer section
with its final CRLF; an explicit `finish` operation may be provided for code
that needs an earlier completion point.

Only trailer names declared when `stream` was selected may be emitted. Trailer
names and values use ordinary HTTP field validation. Fields that affect
framing, routing, authentication, response control, or content processing —
including `transfer-encoding`, `content-length`, `host`, and `trailer` — are
forbidden as trailers. Initial headers remain immutable throughout; trailers
are a separate append-only collection available only in `body_closed` state.

If the producer returns while the body is still open, the adapter closes the
body and finishes an empty trailer section. If it returns after `body.close`,
the adapter only finishes the trailer section. A producer exception or task
cancellation after commitment cannot be converted to a new 500 response; the
adapter aborts the stream/connection, records the failure, and runs exchange
cleanup exactly once.

#### 14.1.4. Streaming request contract

`request.body_stream` is the uniform, single-consumer request body API. Its
`framing` property is fixed before dispatch and is one of `:chunked`,
`:content_length`, or `:empty`. The parser resolves this mode before exposing
any bytes; request method semantics never guess whether a body exists.

Its behavior depends on the selected framing:

- `Transfer-Encoding: chunked` — one `RequestChunk` is yielded for each
  non-terminal wire chunk, preserving its exact semantic boundary and parsed
  extensions;
- valid `Content-Length: N` — exactly N bytes are exposed as bounded
  application chunks; their boundaries are chosen by `read_chunk` demand and
  configured buffer limits, `extensions` is always empty, no trailer fields
  can arrive, and the terminal `trailers` map is empty;
- neither `Transfer-Encoding` nor `Content-Length` — the request body is empty,
  iteration completes immediately, and trailers is an empty immutable map;
- unsupported transfer codings, conflicting framing, or malformed lengths —
  the request is rejected as a protocol error before route dispatch.

Consequently `body_stream` remains useful for large fixed-length uploads; it
does not require the client to choose chunked transfer coding. Code that needs
wire chunk extensions must check `body_stream.framing == :chunked`. A
Content-Length application chunk is not a promise about TCP read boundaries
and is not presented as an HTTP chunk.

The server-side API can consume either framing without first buffering it into
`body_bytes`:

```amber
def ingest(request):
  request.body_stream.each_chunk |chunk|:
    process(chunk.data, extensions: chunk.extensions)

  verify(request.body_stream.trailers)
  ember.render status: :accepted
```

`RequestChunk#data` is `Bytes`. Under chunked framing, `extensions` is the same
validated ordered representation used by the response writer. Transport read
boundaries must never be exposed as wire chunk boundaries: one wire chunk may
arrive in many reads, and one read may contain many wire chunks.

The terminating zero-size chunk is not yielded as application data. Under
chunked framing, after it has been parsed, the body stream becomes closed and
its immutable `trailers` map becomes available. Under Content-Length framing,
the stream closes after exactly N bytes and exposes an empty trailers map.
Accessing trailers before the body reaches its terminal state is an explicit
state error; it must not silently drain unread application data.

`each_chunk` and a single-chunk `read_chunk` operation are cooperative demand
points. The parser reads ahead only within fixed bounds and stops pulling from
the socket when the consumer is not ready. This provides request-side
backpressure and prevents a slow handler from causing unbounded body buffering.

Buffered helpers and streaming consumption are mutually exclusive. Selecting
`body_stream` fixes the request body mode; later `body_bytes`/`body_text` calls
raise a body-state error. Conversely, a buffered helper may consume the stream
only once and remains subject to the configured total body limit. If a handler
abandons an incoming stream, the adapter either drains a separately bounded
amount cooperatively or closes the connection; it never performs an unbounded
background drain.

#### 14.1.5. Full-duplex exchange semantics

Reading the complete request before selecting or producing the response is not
an Ember requirement. HTTP request input and response output are independent
halves of one exchange. A handler may select a `StreamingResponse` immediately,
and its deferred producer may interleave cooperative reads and writes:

```amber
def mirror(request):
  ember.stream(
    status: :ok,
    headers: {"content-type": "application/octet-stream"},
    trailers: ["x-received-chunks"]
  ) |response|:
    received = 0
    request.body_stream.each_chunk |incoming|:
      received += 1
      response.write(incoming.data, extensions: {received: received.to_str})

    response.close
    response.trailer("x-received-chunks", received.to_str)
```

The handler returns the `StreamingResponse` without draining
`request.body_stream`. The adapter then runs its producer while the request
reader remains live. In the example, each completed input chunk can produce an
output chunk while later request chunks are still arriving. A read parks only
on request-side readiness; a write parks only on response-side readiness, so
the reactor can progress either direction and other exchanges independently.

The initial implementation has one logical request-body consumer and one
logical response-body producer. Full duplex means concurrency between those
two halves; it does not make either half an unordered multi-reader or
multi-writer object. Applications needing independent read and produce loops
may connect exchange-owned structured child tasks with a bounded channel, but
all writes still pass through one ordered producer and all reads through one
ordered consumer.

There is no implicit request-body drain before final response headers or body
chunks are sent. If a final response finishes while request bytes remain
unconsumed, the connection is not reusable: the adapter applies the bounded
drain policy or closes it. Future keep-alive support must not place that
connection back in a pool until request framing reaches its terminal state.

`Expect: 100-continue` participates in the same ordering. Selection of a
streaming response freezes its final status and headers logically but does not
by itself force those bytes onto the socket. If the producer first attempts to
read the request body, the adapter sends `100 Continue` before waiting for body
bytes. If the producer first writes or explicitly flushes the final response,
the final response is sent instead and `100 Continue` is suppressed; a client
may then stop uploading. An informational response never reopens frozen final
response metadata.

Backpressure remains bounded in both directions. A client that uploads without
reading and an application that produces without reading can create a genuine
protocol-level stalemate; Ember does not hide it with unbounded buffering.
Configured idle/write/read deadlines and cancellation terminate that exchange
without blocking the scheduler worker.

Full duplex is guaranteed at Ember's direct HTTP connection boundary. A
reverse proxy, gateway, or client library may independently buffer request or
response bodies; deployments that require end-to-end duplex behavior must
disable such buffering and verify the complete path with an integration test.

#### 14.1.6. Framing validation and limits

Incoming and outgoing streaming applies explicit limits at least for:

- chunk-size line bytes;
- extension count and total encoded extension bytes per chunk;
- individual extension name/value length;
- maximum data bytes in one chunk;
- total request body bytes when an application/configuration supplies a cap;
- trailer field count, individual field size, and total trailer bytes;
- bounded read-ahead and write-queue bytes.

Malformed hexadecimal sizes, overflow, invalid extension grammar, missing
CRLF, forbidden trailers, `Content-Length` combined with chunked transfer
coding, and premature EOF are protocol errors. A malformed request is rejected
before dispatch when possible; after dispatch it cancels the request stream and
its exchange. The parser must protect against request smuggling by assigning
one unambiguous framing mode before exposing any body data.

#### 14.1.7. Exchange lifetime, cancellation, and testing

The request exchange remains alive until both the incoming body policy and the
outgoing body producer are finished, cancelled, or aborted. Request-scoped
services, middleware cleanup, and integration scopes therefore cannot be
released merely because the handler returned a `StreamingResponse`. Cleanup is
registered on the exchange and runs once after terminal stream state.

Cancellation propagates in both directions: disconnecting a client wakes a
parked response producer with a stream error, and cancelling the handler wakes
or closes a parked request reader. A blocked writer must not retain a scheduler
worker. Fairness must prevent one continuously writable stream from starving
other runnable exchanges.

The in-memory test adapter preserves semantic chunk boundaries and exposes
status, frozen headers, written chunks with extensions, close state, and
trailers without opening a socket. Low-level loopback tests additionally cover
empty and Content-Length streams, partial framing reads, multiple chunks per
read, full-duplex interleaving, `Expect: 100-continue`, slow producer/consumer
backpressure, cancellation, malformed framing, trailers, and concurrent
progress on one cooperative worker.

---

## 15. Testing and introspection

Most framework tests should not open a socket:

```amber
response = app.call(ember.testing.get("/people/ada-lovelace"))

expect(response.status).to_equal(200)
expect(response.body_text).to_contain("Ada")
```

Required testing surfaces:

- request builders for methods, paths, headers, query, and bodies;
- response status/header/body assertions;
- route match introspection;
- named path generation tests;
- middleware ordering tests;
- function-intrinsic `before` annotation expansion and direct-call tests;
- controller construction isolation;
- controller ORM lease reuse and bare-handler explicit-scope tests;
- historical slug redirect tests;
- CMS catch-all precedence tests;
- 404/405/invalid-path tests;
- low-level loopback integration tests for the adapter only.

`Routes#describe` or a CLI command may render a deterministic route table:

```text
GET  /                    root       Home#landing
GET  /people/:slug       person     People#show
GET  /*path              content    Content#resolve
```

The description is generated from route metadata; it never reflects over
controller decorators.

---

## 16. Compile-time guarantees

The route macro should diagnose before runtime:

- invalid HTTP verb forms;
- non-literal route patterns where static compilation is required;
- malformed params or wildcards;
- duplicate/ambiguous normalized route shapes;
- duplicate route names;
- params declared but absent from the pattern;
- captures missing an explicit typed-param declaration when required by a
  typed route form;
- invalid callable-reference target shapes;
- illegal nested scopes/mounts/resources;
- middleware reference shape mismatches visible to the typed checker.

Generated direct calls allow the binder/checker to diagnose missing functions,
missing instance actions, arity mismatches, and typed parameter mismatches when
the relevant definitions are statically visible and the typed profile is
enabled.

The following remain runtime responsibilities:

- actual path values and decoding failures;
- database slug/path lookup;
- uniqueness conflicts under concurrent writes;
- authorization and publication state;
- ORM/database failures;
- dynamic-profile dispatch failures not statically provable;
- response completion and double-render state.

The design must not claim that a macro can inspect arbitrary future runtime
database state or infer source types from a database schema that is discovered
only after the program starts.

---

## 17. Security requirements

The framework's security baseline includes:

- bounded path, header, query, and body parsing;
- rejection of malformed percent encodings;
- header validation delegated to the low-level HTTP types;
- explicit JSON serialization;
- HTML escaping by context;
- no raw handler names loaded from database data;
- no ambient request or public database-connection lookup; an installed ORM
  integration may use an internal non-inheritable task-local lease inside the
  pool executor;
- production exception responses that do not expose internal messages;
- reserved-path enforcement for root-level slugs;
- tenant/locale scope in dynamic-path lookup and caches;
- database uniqueness as the final slug/path collision guard;
- open-redirect protection for redirect helpers;
- explicit trusted-proxy policy before using forwarded host/scheme data.

Sessions, signed/encrypted cookies, CSRF, multipart limits, secure headers, and
host authorization need dedicated follow-up designs. They should be middleware
or typed helpers over the same request/response core rather than alternative
routing paths.

---

## 18. Project layouts

### 18.1. Single-file application

```amber
package hello

import ember

export main

routes = ember.routes:
  get "/", to: &handler

def handler(request):
  ember.render text: "o hai", status: :ok

def main():
  ember.App(routes: routes).run(port: 3000)
```

### 18.2. MVC-shaped application

```text
app/
  controllers/
    home.am
    people.am
  models/
    person.am
  services/
  views/
config/
  routes.am
app.am
```

`config/routes.am`:

```amber
package app.routes

import ember
from app.controllers.home import Home
from app.controllers.people import People

export routes

routes = ember.routes:
  get "/", to: &Home#landing, as: :root
  get "/people/:slug", to: &People#show, as: :person
```

The project layout is convention. Route binding, handler invocation, request
state, and response semantics are identical to the single-file application.

---

## 19. Implementation phases

### Phase 0 — contract and diagnostics

- fix the `ember.routes` AST/expansion contract;
- fix callable target categories;
- fix route grammar, precedence, and ambiguity diagnostics;
- fix Request/Response and status semantics;
- fix controller lifecycle;
- reserve stable `EMBER_*` diagnostic codes.

### Phase 1 — minimal callable app

- `Routes`, verbs, literal and string-param paths;
- module-function handlers;
- `Request`, `Response`, `render`;
- 404/405;
- `App#call`;
- `net.http.Server` adapter;
- in-memory testing helpers.

### Phase 2 — controllers and middleware

- `ember.Controller`;
- `&Class#action` wrappers;
- status/header/render/redirect helpers;
- application/scope/route middleware;
- exception and request-id middleware.

### Phase 3 — typed params and reverse routing

- `params: {id: Int}` parsing;
- typed generated wrappers;
- route names;
- `Routes#path` and `Routes#url`;
- `scope`, `mount`, and deterministic route description.

### Phase 4 — dynamic content conventions

- reusable slug/path normalization helpers;
- canonical/historical path resolver protocol;
- root-slug reserved-name support;
- optional bounded resolver cache;
- CMS catch-all examples and tests.

The database schema and repository implementation remain application/ORM
responsibilities.

### Phase 5 — HTML and external views

- inline `Html` string-tag macro and escaping model;
- typed Amber-module views;
- compiler-level external source-format RFC;
- `.amv` mounting only after the compiler contract exists.

### Phase 6 — ORM web readiness

- one-time model binding to the ORM pool facade (ORM side implemented);
- controller target connection-scope integration;
- explicit route middleware, `before` annotation, and request-service forms
  for bare handlers;
- model-level transaction helpers (implemented);
- concurrency, cancellation, rollback, and shutdown tests.

### Next phase — cooperative HTTP/1.1 streaming

- immutable status/headers at streaming-body selection;
- automatic `Transfer-Encoding: chunked` and `Trailer` declaration;
- one response write per semantic chunk, with validated chunk extensions;
- body close followed by trailer emission and final trailer close;
- uniform empty, Content-Length, and chunked request streaming;
- exact incoming wire chunks with extensions and post-body trailers when the
  request uses chunked transfer coding;
- full-duplex request-read/response-write progress without a pre-response
  request drain;
- bounded parser/queues and backpressure in both directions;
- cooperative parking, cancellation, fairness, and exchange-scoped cleanup;
- in-memory semantic tests and real loopback framing/concurrency tests.

### Later phases

- `resources` and generators;
- sessions, cookies, flash, CSRF;
- multipart/form handling;
- TLS/HTTP protocol upgrades through lower layers;
- development reload based on package/module rebuilds;
- optional atomically swappable declarative route snapshots.

---

## 20. Decisions fixed by this document

1. The framework and working package name is Ember.
2. The only route-table constructor spelling is `ember.routes`.
3. The canonical use is `routes = ember.routes:` and returns immutable
   `ember.Routes`.
4. Routes are central and never declared by handler annotations.
5. Separate route files are static Amber modules, not Ruby-style runtime loads.
6. Function targets use `&handler`; controller actions use `&Class#method`.
7. `&Class.method` retains its Amber class-side meaning.
8. Controllers are per-request instances.
9. Free handlers construct responses explicitly; there is no ambient current
   response.
10. Dynamic entity URLs are static route patterns plus database resolution.
11. Slug changes update canonical/history data and may redirect old paths.
12. Arbitrary CMS URLs use a lowest-precedence database-backed catch-all.
13. Routes remain immutable when slug/path data changes.
14. Core Ember is ORM-neutral.
15. Web ORM integration binds models once to a shareable pool-backed executor.
16. Controller targets automatically receive a scoped lease when the optional
    integration is installed; controller actions continue to use `Person.find`
    and record methods.
17. Bare function handlers receive no hidden controller/ORM scope; middleware,
    a `before` annotation, or an explicit request service supplies it.
18. Typed view calls are preferred; external `.amv` mounting depends on a
    compiler-level source-format registry.
19. Selecting a streaming response immediately freezes status and initial
    headers, before the first body write.
20. HTTP/1.1 streaming responses own their framing and automatically use
    `Transfer-Encoding: chunked`; they never combine chunked coding with
    `Content-Length`.
21. Every response stream write is one semantic chunk and may carry validated
    RFC chunk extensions.
22. Response trailers are declared up front and are emitted only after the
    data body has been closed.
23. `request.body_stream` is uniform across empty, Content-Length, and chunked
    bodies; only chunked framing promises exact wire boundaries, extensions,
    and trailers.
24. A streaming response producer may read the still-open request body and
    write response chunks without a mandatory request drain; both directions
    progress cooperatively as one full-duplex exchange.
25. Request and response streaming are bounded, backpressured, cancellable,
    and cooperative from the first implementation.
26. Request/integration lifetime extends through terminal stream state; it does
    not end merely when a handler returns a streaming response.
27. `Expect: 100-continue` is decided by exchange ordering: a first body read
    may send 100, while a first final-response write suppresses it.

---

## 21. Remaining open questions

1. Whether automatic HEAD-from-GET is enabled by default.
2. Whether trailing slash policy defaults to strict match or canonical
   redirect.
3. Whether typed path params keep the explicit `params: {id: Int}` form or gain
   additional path-literal sugar later.
4. Whether permanent historical-slug redirects default to 308 globally or use
   301 specifically for GET/HEAD applications.
5. The exact normalized Unicode/case policy supplied by optional slug helpers.
6. The final spelling and lifecycle ordering of `App#integrations` relative to
   application middleware and shutdown hooks.
7. The manifest and provider-reference syntax for external `.amv` formats.
8. The minimum low-level server work required before Ember is described as
   production-ready rather than an application-framework preview.
