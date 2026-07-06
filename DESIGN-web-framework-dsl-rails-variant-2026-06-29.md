# SKETCH: Rails-like MVC variant for Ember

Date: 2026-06-29
Status: first-approximation sketch — exploratory, not a spec. A *variant* of
`DESIGN-web-framework-dsl-sketch-2026-06-29.md` (the minimal Sinatra-style
surface). Depends on the macro system in `DESIGN-macro-system-2026-06-29.md`
(unimplemented). Surface syntax is illustrative.
Scope: a convention-over-configuration, Rails-shaped variant — a dedicated
`config/routes.am` *draw* file, RESTful `resources`, and a full
Model → Service → Controller → View flow with service objects. For each
construct, shows what it expands to, so the whole stack stays typed and is
validated at compile time rather than at first request.
Builds on: the macro trigger surfaces (macro doc §8), the **real** `Result[T,E]`
type (Ok/Err/`.or`/`.or_raise`/`.map`, spec §10.6 — implemented; see the
result-type memory), `case!` exhaustiveness in the typed profile (Q4), the
capability model, strand confinement, and the assumed `net.http` server surface.

What is real vs assumed: `Result[T,E]`, `case!`, capabilities, strands, and the
macro substrate are grounded. The **ORM/`Model` layer, a `db` capability, and
the view/template engine are assumed** — they do not exist in the runtime today
and are sketched only enough to show the flow.

---

## 1. How this differs from the minimal sketch

| Axis            | Minimal (Sinatra)            | This variant (Rails)                 |
|-----------------|------------------------------|--------------------------------------|
| Routing         | `@route` on each handler     | central `config/routes.am` `draw:`   |
| Convention      | explicit per route           | `resources :posts` ⇒ 7 routes        |
| Layering        | handler does everything      | Model / Service / Controller / View  |
| Business logic  | inline in handler            | **service objects returning `Result`** |
| Wiring check    | per-handler                  | route↔action checked at compile time |

The defining bet: **the layering is enforced by macros at expansion time.** A
route that names a missing controller action, a strong-params call that names a
field absent from the model schema, or a view that reads an unassigned variable
are all *compile-time diagnostics*, not 500s on first traffic. Rails resolves
these at runtime; the macro pass resolves them before the binder runs.

## 2. Project layout (convention)

```text
app/
  models/        post.am            user.am
  services/      create_post.am     publish_post.am
  controllers/   posts_controller.am
  views/         posts/index.amv    posts/show.amv
config/
  routes.am
```

The build profile (`[profile] macros = true`, web = true) tells the compiler to
treat `config/routes.am` as the route-draw entry and `app/**` by convention.

## 3. The routes draw file — `config/routes.am`

`draw:` is a block-suffix macro (macro doc §8.2). Its whole body is consumed as
AST at compile time; nothing here runs at request time.

```amber
ember.routes.draw:
  root to: "home#index"

  resources :posts:
    member:
      post "publish" -> posts#publish
    collection:
      get "drafts" -> posts#drafts
    resources :comments, only: [:index, :create]

  namespace :api:
    resources :users, only: [:index, :show]

  get "/health" -> system#health
  constraints host: "admin.example.com":
    resources :flags
```

`resources :posts` expands to the seven RESTful registrations, each bound to a
**callable reference** (spec §`&target`) of a typed controller action:

```amber
# --- after expansion (illustrative) ---
ember.__route(t, GET,    "/posts",          &PostsController.index)
ember.__route(t, GET,    "/posts/new",      &PostsController.new_)
ember.__route(t, POST,   "/posts",          &PostsController.create)
ember.__route(t, GET,    "/posts/:id",      &PostsController.show)
ember.__route(t, GET,    "/posts/:id/edit", &PostsController.edit)
ember.__route(t, PATCH,  "/posts/:id",      &PostsController.update)
ember.__route(t, DELETE, "/posts/:id",      &PostsController.destroy)
ember.__route(t, POST,   "/posts/:id/publish", &PostsController.publish)
# nested + namespaced routes expand with concatenated path + module prefix
```

**Compile-time payoff:** the `resources` macro resolves each `posts#action` to
`&PostsController.action`. If `PostsController` lacks `publish`, expansion emits
`AMB_ROUTE_NO_ACTION` with the `routes.am` span — a missing route target is a
build error, not a runtime `NoMethodError`.

## 4. Models — `app/models/post.am`

`use ember.Model` is an injection macro (macro doc §8.4); `schema`/
`belongs_to`/`has_many`/`validates`/`scope` are macros that read their forms and
emit typed accessors, association loaders, a validator, and named queries.

```amber
class Post:
  use ember.Model

  schema:
    title:     Str
    body:      Str
    published: Bool = false
    author_id: Int

  belongs_to :author, Models.User
  has_many   :comments, Models.Comment

  validates :title, presence: true, length: 3..120
  validates :body,  presence: true

  scope :published, -> q: q.where(published: true)
```

`schema:` expands to typed fields + a `from_row`/`to_row` pair over the assumed
`db` layer; `validates` expands to a typed `validate(self) -> Result[Self,
Errors]`; `has_many :comments` expands to a typed `comments(self) ->
Query[Comment]`. No runtime field reflection — every accessor is a real method.

The `Model` base provides the repository surface (`Post.find(id) ->
Result[Post, NotFound]`, `Post.where(...)`, `.save -> Result[Post, Errors]`),
all capability-gated on a `db` handle passed to the repository, never ambient.

## 5. Services — `app/services/create_post.am`

Service objects are plain Amber holding business logic, returning the **real
`Result[T,E]`** so the controller never deals in exceptions for expected
failures. This is the layer that most benefits from Amber already having
`Result` and `case!` exhaustiveness.

```amber
class CreatePost:
  def init(repo: PostRepo, author: User):
    @repo = repo
    @author = author

  # returns Result[Post, CreatePostError]
  def call(attrs: PostAttrs) -> Result[Post, CreatePostError]:
    post = Post.new(
      title: attrs.title, body: attrs.body, author_id: @author.id)

    post.validate
      .map_err |errs|: CreatePostError.Invalid(errs)
      .and_then |valid|: @repo.save(valid)
        .map_err |e|: CreatePostError.Persist(e)
```

`CreatePostError` is a tagged union (`Invalid(Errors) | Persist(DbError)`), so
the controller can `case!` it exhaustively. The service does the work; it knows
nothing about HTTP. That separation is what "service object" buys, and `Result`
makes the error channel explicit instead of a rescue ladder.

## 6. Controllers — `app/controllers/posts_controller.am`

```amber
class PostsController:
  use ember.Controller

  before_action :require_login, except: [:index, :show]
  before_action :load_post,     only:   [:show, :edit, :update, :destroy]

  def index(req):
    posts = Post.published.all
    render :index, posts: posts

  def create(req):
    attrs = params(req).require(:post).permit(:title, :body)   # strong params
    service = CreatePost.new(repo: req.repo, author: current_user(req))

    case! service.call(attrs):
      Ok(post):
        redirect to: post_path(post.id), notice: "created"
      Err(CreatePostError.Invalid(errs)):
        render :new, errors: errs, status: 422
      Err(CreatePostError.Persist(_)):
        render :error, status: 500
```

- `use ember.Controller` injects `render`, `redirect`, `params`, `current_user`,
  `post_path`, etc. as real typed methods (no `method_missing`).
- `before_action :require_login, except: [...]` expands at compile time into the
  filter chain that wraps each named action — a straight-line composition the
  type checker sees, not a runtime fold over `Any`.
- `params(req).require(:post).permit(:title, :body)` is a macro-checked strong-
  params builder: because the macro knows `Post`'s schema (§4), permitting a
  field the schema lacks is `AMB_PARAMS_UNKNOWN_FIELD` at build time.
- `case!` is exhaustive (Q4): if `CreatePostError` grows a variant and a
  controller forgets it, that is a compile error.

## 7. Views — `app/views/posts/index.amv`

A view is its own small macro DSL: the template compiles to a typed `render`
function whose parameters are the assigns the controller passes.

```text
# posts/index.amv  (assigns: posts: List[Post])
<h1>Posts</h1>
<ul>
  {% for p in posts %}
    <li><a href="{{ post_path(p.id) }}">{{ p.title }}</a></li>
  {% end %}
</ul>
```

The template compiler (a macro over the assigns + the `.amv` source) emits:

```amber
def render_posts_index(posts: List[Post]) -> Html:
  out = Html.builder
  out.raw("<h1>Posts</h1><ul>")
  for p in posts:
    out.raw("<li><a href=\"")
    out.text(post_path(p.id)); out.raw("\">")
    out.text(p.title); out.raw("</a></li>")
  out.raw("</ul>"); out.build
```

**Compile-time payoff:** `{{ p.title }}` is checked against `Post`; a typo
(`{{ p.titel }}`) is a build error, and `{{ }}` text interpolation auto-escapes
(`out.text`) while `{% raw %}` does not — XSS-safe by construction.

## 8. End-to-end request flow

`POST /posts` threads through the layers; `Result` carries the expected-failure
channel from the service up to the controller's `case!`:

```text
request  -> router (built table)  -> before_action chain
         -> PostsController.create -> strong params (schema-checked)
         -> CreatePost service     -> Post model / repo (db capability)
         -> Result[Post, CreatePostError]
         -> case!  Ok -> redirect ;  Err -> render view (422/500)
```

Each arrow is generated typed Amber; the only runtime indirection is the route
table lookup. See the diagram in the chat response for the layered view.

## 9. Where the macros earn their keep (compile-time validation)

| Check                                   | Rails        | Ember (this variant) |
|-----------------------------------------|--------------|----------------------|
| Route names a missing controller action | runtime 500  | build error          |
| Strong params permit unknown field      | silent drop  | build error          |
| View reads an unassigned variable       | runtime nil  | build error          |
| Service error variant unhandled         | runtime path | non-exhaustive `case!` build error |
| Auto-escaping in views                  | per-helper   | default in `{{ }}`   |

This is the whole reason to express MVC as macros rather than runtime
metaprogramming: the convention-over-configuration ergonomics of Rails, but the
wiring is checked by the typed profile (Q4) before the program runs, and there
is no `method_missing`/`Any`-boundary tax per request (macro doc §1 table).

## 10. Open ends (sketch-level)

- **ORM reality.** The `Model`/repository layer assumes a `db` capability and a
  query builder that do not exist; this sketch only shows the macro-facing
  shape. A real design needs migrations, a connection pool over the reactor, and
  a `Query[T]` type. Large, separate effort.
- **Template engine.** `.amv` compilation is itself a macro DSL; partials,
  layouts, helpers, and streaming are unspecified here.
- **`before_action` data flow.** How `load_post` hands `@post` to the action
  hygienically (macro doc §9 — likely an `unhygienic`/assign-into-instance form,
  audited).
- **Background jobs / mailers** — the other Rails layers — would each be their
  own macro DSL in the same style.
- **Routes DSL coverage**: `concern`, `scope module:`, route globbing, and
  format constraints are sketched only by example.
- **Names**: "Ember", `.amv`, and every keyword here are placeholders.
