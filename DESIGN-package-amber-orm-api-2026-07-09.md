# DESIGN - `amber-orm` package public API

Status: P0 implemented; design remains incremental  
Date: 2026-07-09  
Target: external Amber package `amber-orm`  
Scope: model declaration surface, schema/attribute layer, dirty tracking,
validation, persistence Result API, adapter protocol, SQLite glue, tableless
models, and a minimal lazy query relation  
Out of scope: migrations, associations, joins and advanced query algebra,
connection pools, optimistic locking, callbacks, eager loading, code generation
for every database dialect, and a production migration planner

## 0. Executive Summary

`amber-orm` is a small, ActiveRecord-shaped modeling package for Amber. It is
not intended to hide SQL or turn database access into runtime magic. Its first
goal is narrower:

1. Define model attributes from either database schema introspection or explicit
   model declarations.
2. Track local changes precisely.
3. Validate records before persistence.
4. Persist records through a small adapter protocol.
5. Let the same model behavior work without a database through tableless
   execution.

The package should be a separate repository next to `sqlite3-amber`:

```text
workspace/
  amber-lang/
  sqlite3-amber/
  amber-orm/
```

The first adapter target is `sqlite3-amber`. The core package should not know
about SQLite-specific APIs except in a dedicated `orm.sqlite3` glue module.

The intended user experience is ActiveRecord-ish, but explicit at the edges:

```amber
import orm
import orm.sqlite3 as orm_sqlite

class User:
  use orm.model:
    expect_schema:
      column(:id, type: :integer, primary_key: true, generated: true)
      column(:email, type: :text, nullable: false)
      column(:age, type: :integer, nullable: true)

    validate:
      expect(:email, email.present?, message: "must be present")
      expect(:age, age.absent? or age >= 0,
        code: :gte, message: "must be greater than or equal to 0")

User.bind!(orm_sqlite.adapter(db)).or_raise

user = User(email: "ada@example.test", age: 36)
user[:email] = "ada@amber.test"
user.changed?       # true
user.changes        # {email: {from: "ada@example.test", to: "ada@amber.test"}}
user.save.or_raise  # returns user or raises the Err payload
```

The persistence surface follows Amber's `Result[T, E]` convention. There are no
raising `save!` / `create!` aliases. `!` stays reserved for local mutation:

```amber
user.save              # Ok(user) | Err(Orm.ValidationError / Orm.PersistenceError)
user.save.or_raise
user.assign!(attrs)    # mutates local attributes, returns self
user.clear_changes!    # mutates local dirty state, returns self
```

## 1. Design Anchors

1. **ActiveRecord direction, not ActiveRecord magic.** A model can know its table
   and save itself, but schema binding, adapter binding, and mismatch policy are
   explicit.
2. **Attributes are the foundation.** Persistence, validations, serialization,
   and dirty tracking are layered over the same attribute storage.
3. **Validations live in one section.** A model has one `validate:` block with
   `expect` statements, instead of Rails-style validations spread through the
   class body.
4. **Inference is the default; expectation is explicit.** A bare model adopts
   the bound table schema. An optional `expect_schema:` block declares the
   exact schema the adapter must report and fails binding when it differs.
5. **Dirty tracking is local and deterministic.** Attribute writers record
   before/after values with no hidden database reads.
6. **Validation failures are expected values.** `save` returns `Err`, and
   `.or_raise` is the user's explicit escalation path.
7. **Execution is pluggable.** A model may be backed by SQLite, another future
   DBMS, or no DBMS at all.
8. **Tableless is not a fake model.** A tableless model should behave like a
   normal model for attributes, dirty tracking, validations, and `save`, but
   its executor chooses what "persisted" means.
9. **Adapters expose capabilities, not raw internals.** ORM core talks to an
   adapter protocol: schema introspection and CRUD-shaped commands.
10. **P0 uses ordinary indexing for generic attributes.** Expected, inferred,
    and tableless attributes share `record[name]`, `record[name] = value`, and
    `assign!`; optional generated accessors remain a later convenience rather
    than a schema-mode distinction.
11. **SQL remains inspectable.** The SQLite adapter can use `sqlite3-amber`'s
    safe builders and fragments; the ORM core does not concatenate arbitrary
    user SQL.
12. **Declarations stay grouped.** `column` belongs to `expect_schema:`,
    `attribute` belongs to tableless `schema:`, and `expect` belongs to
    `validate:`. Loose declarations are rejected.

## 2. Package Shape

The root module id should be `orm`.

Canonical imports:

```amber
import orm
import orm.sqlite3 as orm_sqlite

from orm import Model, Column, Schema, ValidationErrors
```

Proposed exports:

```amber
package orm

export model, tableless
export Model, TablelessModel
export AttributeSpec, Column, Schema, SchemaDiff
export AttributeSet, ChangeSet
export ValidationErrors, ValidationIssue
export Executor, TablelessExecutor
export SaveResult, DeleteResult
export Error, SchemaError, SchemaMismatchError, UnknownAttributeError
export ValidationError, PersistenceError, NotFoundError, StaleRecordError
```

SQLite glue:

```amber
package orm.sqlite3

import sqlite3
import orm

export adapter, SqliteAdapter
```

Repository layout:

```text
amber.build.yaml
src/
  orm.am
  orm/sqlite3.am
test/
  selftest.am
  selftest.build.yaml
examples/
  sqlite_basic.am
  tableless.am
```

The package should remain Amber-source-only at first. `orm.sqlite3` depends on
the external `sqlite3` package but adds no native code of its own.

## 3. Model Declaration Surface

### 3.1 Optional Schema Expectation

A table-backed model infers its columns from the adapter by default. Production
code that wants a checked contract adds `expect_schema:`. The nested `column`
declarations are expectations, not an alternative source of unchecked column
metadata: binding introspects the table and compares the actual schema exactly.

```amber
class User:
  use orm.model:
    expect_schema:
      column(:id, type: :integer, primary_key: true, generated: true)
      column(:email, type: :text, nullable: false)
      column(:age, type: :integer, nullable: true, default: null)

    validate:
      expect(:email, email.present?, message: "must be present")
```

Expansion intent:

```amber
class User:
  include orm.ModelInstance

  def init(attrs = {}, **kwargs):
    self._init_orm_instance!(User.orm_model, attrs.merge(kwargs))

  class_method def orm_model():
    if @@orm_model:
      @@orm_model
    else:
      @@orm_model = orm.__model(
        name: "User",
        table: :users,
        declarations: [...])
      @@orm_model.set_record_factory! |attrs|:
        User(attrs)
      @@orm_model
```

`column` is valid only while executing an `expect_schema:` macro block. A bare
`column` in the `orm.model` body, or any `column` in `orm.tableless`, is a
located macro diagnostic. This keeps the word tied to its real purpose: matching
a DBMS column contract.

The macro blocks compose by execution. `model` invokes its `&blk`; nested
`expect_schema` and `validate` macros invoke their own `&blk`; `column` and
`expect` return quoted runtime descriptors. The package does not inspect raw
`AstPostfixChain` / `AstTailBlockSuffix` parser shapes.

#### 3.1.1 Table Naming

The `table:` option is optional for conventional table names:

```amber
class User:
  use orm.model

class AuditLog:
  use orm.model
```

Default table names:

| Class | Default table |
| --- | --- |
| `User` | `:users` |
| `AuditLog` | `:audit_logs` |
| `Person` | `:people` if bundled inflections include it, otherwise override |

Amber allows this in the macro surface: a `use` injection macro receives the
enclosing class declaration AST and can read the class name at expansion time.
The default table name is therefore compile-time macro output, not runtime
reflection.

The inflector must be deterministic and small in P0:

1. convert `CamelCase` to `snake_case`;
2. apply a tiny built-in irregular map for common words if we choose to ship
   one;
3. otherwise append `s`;
4. require explicit `table:` for anything surprising.

Examples:

```amber
class NewsItem:
  use orm.model(table: :news):
    ...

class PeopleDirectory:
  use orm.model(table: :people_directories):
    ...
```

### 3.2 Inferred Schema

Inference is the unadorned model behavior: it reads column metadata from the
bound adapter when no `expect_schema:` descriptor was emitted.

```amber
class User:
  use orm.model

User.bind!(orm_sqlite.adapter(db)).or_raise

user = User.find(1).or_raise
user[:email]
user.write!(:email, "ada@example.test")
```

P0 rule: inferred columns do not generate static accessors. They populate the
model's runtime `Schema`, and callers use:

```amber
user[:email]
user[:email] = "ada@example.test"
user.read(:email)
user.write!(:email, "ada@example.test")
user.assign!(email: "ada@example.test")
```

Later, the package may add:

```amber
User.generate_accessors!.or_raise
```

or a compile-time schema file, but runtime method generation should not be part
of P0. This keeps inferred schema useful without crossing into surprising MOP
behavior.

### 3.3 Tableless Schema

Tableless models declare attributes explicitly but omit a DBMS table.

```amber
class SignupForm:
  use orm.tableless:
    schema:
      attribute(:email, type: :text)
      attribute(:password, type: :text)

    validate:
      expect(:email, email.present?, message: "must be present")
      expect(:password, password.present?, message: "must be present")
      expect(:password, password.absent? or password.length >= 12,
        code: :length, message: "length must be at least 12")

form = SignupForm(email: params[:email], password: params[:password])
form.save # Ok(form) or Err(Orm.ValidationError)
```

Tableless models should share the same attribute, dirty, validation, and Result
surface as table-backed models. The executor simply avoids DB writes.

The tableless DSL deliberately says `schema:` and `attribute`, not `column`. A
`column` declaration means "this model attribute is backed by a database column
and participates in schema diffing." An `attribute` declaration means "this
model has typed attribute storage, dirty tracking, and validations, but no DBMS
column." `attribute` is valid only inside the tableless `schema:` section;
loose attributes and tableless columns are diagnostics.

Tableless examples should read like form or service objects:

```amber
class SignupForm:
  use orm.tableless:
    schema:
      attribute(:email, type: :text)
      attribute(:password, type: :text)
      attribute(:accepted_terms, type: :bool, default: false)

    validate:
      expect(:email, email.present?, message: "must be present")
      expect(:password, password.present?, message: "must be present")
      expect(:password, password.absent? or password.length >= 12,
        code: :length, message: "length must be at least 12")
      expect(:accepted_terms, accepted_terms == true,
        message: "must be accepted")

class SearchParams:
  use orm.tableless(persisted: false):
    schema:
      attribute(:query, type: :text)
      attribute(:page, type: :integer, default: 1)
```

### 3.4 Non-Macro Factory

The same descriptors remain available directly for dynamic model factories:

```amber
User = orm.model(name: :User,
  table: :users,
  expected_schema: orm.ExpectedSchema([
      orm.column(:id, type: :integer, primary_key: true, generated: true),
      orm.column(:email, type: :text, nullable: false)
    ]))

SignupForm = orm.tableless(name: :SignupForm,
  schema: orm.AttributeSchema([
    orm.attribute(:email, type: :text),
    orm.attribute(:password, type: :text)
  ]))
```

This is useful for runtime composition; class-local declarations should prefer
the macro surface.

## 4. Schema and Column Model

### 4.1 Column

`Column` is a small immutable value:

```amber
class Column:
  attr name
  attr type
  attr nullable
  attr primary_key
  attr generated
  attr default
  attr db_type
```

Constructor intent:

```amber
orm.Column(
  :email,
  type: :text,
  null: false,
  primary_key: false,
  generated: false,
  default: null,
  db_type: "TEXT"
)
```

Common logical types:

| ORM type | Amber values | SQLite affinity |
| --- | --- | --- |
| `:integer` | `Int`, `Bool` if configured | `INTEGER` |
| `:float` | `Float`, `Int` accepted by cast policy | `REAL` |
| `:text` | `Str` | `TEXT` |
| `:binary` | `Bytes` | `BLOB` |
| `:bool` | `Bool` | `INTEGER` |
| `:json` | `Map`, `Array`, scalar JSON values | `TEXT` |
| `:datetime` | future `Time` value | `TEXT` or `INTEGER` by adapter policy |
| `:any` | any bindable value | adapter-specific |

P0 should support `:integer`, `:float`, `:text`, `:binary`, `:bool`, and `:any`.
JSON/time can be designed but deferred.

### 4.2 Schema

`Schema` owns ordered columns and lookup helpers:

```amber
class Schema:
  def columns()
  def column_names()
  def column(name)
  def column?(name)
  def primary_key()
  def generated_columns()
  def writable_columns()
  def insert_columns()
  def update_columns()
```

`Schema` should use name-indifferent maps for public lookups, while preserving
the original column order for SQL generation and deterministic tests.

### 4.3 Schema Binding

Binding connects a model class to an executor:

```amber
User.bind!(orm_sqlite.adapter(db)).or_raise
```

`bind!` mutates class-level model state and returns:

```amber
Ok(User) | Err(Orm.SchemaError)
```

When `expect_schema:` is present:

1. Fetch actual table schema from the adapter.
2. Normalize DB columns into `Column` values.
3. Diff expected vs actual schema.
4. If the diff is unacceptable, return `Err(SchemaMismatchError(diff))`.
5. Store the checked actual schema plus adapter metadata.

For inferred schema:

1. Fetch actual table schema.
2. Normalize into `Schema`.
3. Store the inferred schema.

For tableless:

1. Store declared schema.
2. Bind `TablelessExecutor`.
3. No database introspection.

### 4.4 Schema Diff Policy

Expected schemas should be strict enough to catch mistakes but not so strict
that harmless DB details break the app.

P0 mismatch checks:

| Difference | Result |
| --- | --- |
| declared column missing in DB | mismatch |
| DB column missing in declaration | mismatch by default |
| primary key differs | mismatch |
| nullability differs | mismatch |
| generated flag differs | mismatch |
| logical type incompatible | mismatch |
| raw `db_type` spelling differs but maps to same logical type | ok |
| DB default differs from declared default | warning in P0, configurable later |

The default should be exact declarations. A later option can allow DB extras:

```amber
use orm.model(table: :users):
  expect_schema(extra_columns: :ignore):
    ...
```

`SchemaDiff` should be structured, not just a message:

```amber
{
  table: "users",
  missing_in_db: [:email],
  missing_in_model: [:created_at],
  incompatible: [
    {column: :age, declared: orm.Column(...), actual: orm.Column(...), reason: :nullability}
  ]
}
```

## 5. Attribute Layer

### 5.1 Storage

Every model instance owns an `AttributeSet`:

```amber
class AttributeSet:
  def init(schema, values: {}, persisted: false)
  def [](name)
  def []=(name, value)
  def assign!(attrs)
  def to_map()
```

The model delegates:

```amber
user[:email]
user[:email] = "ada@example.test"
user.assign!(email: "ada@example.test")
user.attributes
```

Unknown attributes raise or return `Err` depending on context:

| API | Unknown attribute |
| --- | --- |
| `[]` | raises `UnknownAttributeError` |
| `[]=` | raises `UnknownAttributeError` |
| constructor with unknown keyword | raises `UnknownAttributeError` |
| `try_assign` if added later | `Err(UnknownAttributeError)` |

This mirrors ordinary Amber indexing and assignment: local mutation helpers can
raise programmer errors, while persistence methods return `Result` for expected
domain/DB failures.

### 5.2 Construction

Declared models should be constructible from keyword arguments:

```amber
user = User(email: "ada@example.test", age: 36)
```

Inferred models cannot know accepted keyword arguments at compile time. They can
still accept keyword rest:

```amber
user = User.new(**attrs)
```

Implementation shape:

```amber
def init(attrs = {}, **kwargs):
  self._init_orm_instance!(User.orm_model, attrs.merge(kwargs))
```

The constructor fills missing attributes from column defaults where the default
is an Amber-side value. DB-side generated/default columns remain absent or
`null` until reload/insert returning.

### 5.3 Type Coercion

P0 should prefer validation over automatic coercion. Attribute writes accept
values that are already compatible with the column's logical type.

Optional explicit cast policy:

```amber
column :age, type: :integer, cast: true
```

Without `cast: true`, `"42"` assigned to an integer column is a validation error
or type error, not silently converted.

## 6. Dirty Layer

Dirty tracking is based on the persisted snapshot plus current values.

Each model instance tracks:

```amber
@__orm_persisted      # Bool
@__orm_destroyed      # Bool
@__orm_original_attrs # Map of last persisted values
@__orm_attrs          # current AttributeSet
```

Public surface:

```amber
user.new_record?
user.persisted?
user.destroyed?

user.changed?
user.changed?(:email)
user.changed_attributes
user.changes
user.previous_changes

user.clear_changes!
user.mark_persisted!(values: null)
user.restore_attribute!(:email)
user.restore_attributes!
```

Change representation:

```amber
user.changes
# {
#   email: {from: "old@example.test", to: "new@example.test"},
#   age: {from: 36, to: 37}
# }
```

Rules:

1. A newly constructed record starts clean; `new_record?` and `changed?` are
   independent state.
2. Writing the same value does not mark a change.
3. Writing a value back to its original persisted value removes that change.
4. `mark_persisted!` replaces the original snapshot with current values and
   clears changes.
5. `save` calls `mark_persisted!` after successful insert/update.
6. `previous_changes` records the last successful persistence change set.
7. P0 snapshots the attribute map with `Map#copy`, not `deep_copy`. Mutating a
   nested value in place is therefore not tracked; assign a replacement value
   through the record to make the change observable. A future typed attribute
   may provide explicit in-place tracking, but generic dirty state must not
   duplicate user instances or opaque resources merely to take a snapshot.

The dirty layer should not read the database to decide whether a value changed.
It is purely local.

## 7. Validation Layer

### 7.1 Validation Errors

Validation failures are collected into a structured value:

```amber
class ValidationIssue:
  attr attribute
  attr code
  attr message
  attr source
  attr value
  attr meta

class ValidationErrors:
  def empty?()
  def count()
  def add!(attribute, code:, message:, source: null, value: null, meta: {})
  def on(attribute)
  def to_map()
```

`Orm.ValidationError` wraps `ValidationErrors`:

```amber
err = Orm.ValidationError(errors)
err.errors
err.message
```

### 7.2 Single Validation Block

The public validation declaration surface is one `validate:` block per model.
Validations should not be scattered among attributes, associations, callbacks,
or helper methods the way Rails permits. A model should have a clear shape:
schema first, then one local validation section.

```amber
class User:
  use orm.model:
    expect_schema:
      column(:id, type: :integer, primary_key: true, generated: true)
      column(:email, type: :text, nullable: false)
      column(:age, type: :integer, nullable: true)

    validate:
      expect(:email, email.present?, message: "must be present")
      expect(:age, age.absent? or age >= 0,
        code: :gte, message: "must be greater than or equal to 0")
```

`validate:` is a macro-only class-body block consumed by `orm.model` /
`orm.tableless`. The block may appear at most once in a model declaration.
Repeated `validate:` blocks, or a bare class-body `validate email.present?`,
should be macro diagnostics. The runtime instance method remains
`user.validate`.

Inside the block, `expect` is a nested macro. It receives an explicit attribute
symbol and an unevaluated predicate expression. The expression remains the rule;
there is no `presence`, `comparison`, `length_between`, `inclusion`, or
ActiveRecord-style option vocabulary.

`validate` invokes its `&blk`, receives the quoted validation descriptors
emitted by `expect`, and wraps them for the model declaration. It does not walk
the parser AST of the block.

### 7.3 Expect Predicate Macro

The macro should keep the generated runtime boring. Conceptually:

```amber
validate:
  expect(:age, age.absent? or age >= 0,
    code: :gte, message: "must be greater than or equal to 0")
```

emits a validation spec equivalent to:

```amber
orm.PredicateValidation(
  on: :age,
  source: "age.absent? or age >= 0",
  code: :gte,
  message: "must be greater than or equal to 0",
  predicate: |record|: record.age.absent? or record.age >= 0
)
```

The attribute is always explicit. `code` and `message` are optional; without a
message, the fallback uses the source expression:

```amber
validate:
  expect(:ends_at, starts_at < ends_at, message: "must be after start")
  expect(:password_confirmation, password == password_confirmation,
    message: "does not match")
```

`present?` and `absent?` are ordinary Amber left-to-right predicates. They mean:

| Value | Present? |
| --- | --- |
| `null` | false |
| `false` | false |
| `true` | true |
| `Str` | `value.length > 0` |
| `Array`, `Map`, `Set`, `Tuple` | `count > 0` |
| everything else | true |

User-defined classes may override either predicate; otherwise their instances
are present.

The fallback is intentionally source-based rather than pretending to infer a
domain vocabulary:

```amber
validate:
  expect(:ends_at, starts_at < ends_at)
# default issue:
#   attribute: :base or :ends_at if inferred
#   code: :invalid
#   message: "must satisfy starts_at < ends_at"
#   source: "starts_at < ends_at"
```

When the fallback is not good enough, users provide the clean message at the
validation site:

```amber
validate:
  expect(:ends_at, starts_at < ends_at, message: "must be after start")
```

This deliberately replaces:

```amber
validates :age, numericality: {gte: 0}, allow_null: true
```

with:

```amber
validate:
  expect(:age, age.absent? or age >= 0,
    code: :gte, message: "must be greater than or equal to 0")
```

The expression is longer than a keyword, but much shorter than the configuration
object and easier to read in bulk.

For rules that need several checks or custom error placement, keep them in the
same block:

```amber
validate:
  expect(:email, email.present?, message: "must be present")
  expect(:age, age.absent? or age >= 0,
    code: :gte, message: "must be greater than or equal to 0")
  expect(:password_confirmation, password == password_confirmation,
    message: "does not match")
```

The block gives the model one validation section to execute at expansion time.
P0 accepts only `expect` emissions in that block; arbitrary compile-time helper
code can be considered after the callable-block contract has settled.

### 7.4 Validation Execution

Public methods:

```amber
user.valid?
user.validate
user.errors
```

`validate` returns:

```amber
Ok(user) | Err(Orm.ValidationError)
```

`valid?` is convenience:

```amber
def valid?():
  validate.ok?
```

`save` always calls `validate` unless passed `validate: false`:

```amber
user.save(validate: false)
```

Skipping validations should be explicit and visible because it bypasses the
domain layer.

## 8. Execution Layer

### 8.1 Result-Returning Persistence

Persistence methods return `Result`:

```amber
user.save                  # Ok(user) | Err(Error)
user.insert                # Ok(user) | Err(Error)
user.update                # Ok(user) | Err(Error)
user.delete                # Ok(user) | Err(Error)
user.reload                # Ok(user) | Err(Error)

User.find(id)              # Ok(user) | Err(NotFoundError / PersistenceError)
User.create(attrs)         # Ok(user) | Err(ValidationError / PersistenceError)
User.where(where).all      # Ok([user, ...]) | Err(PersistenceError)
```

The first class-side query surface stays deliberately small:

```amber
User.find(id)
User.get(where: {email: "ada@example.test"})
User.where({email: "ada@example.test"}).all

adults = User.where |query|:
  query.field(:age) >= 18

adults.first
```

`save` algorithm:

1. If destroyed, return `Err(PersistenceError(...))`.
2. Validate unless `validate: false`.
3. If invalid, return `Err(ValidationError(errors))`.
4. If tableless, call tableless executor.
5. If new record, insert.
6. If persisted and unchanged, return `Ok(self)`.
7. If persisted and changed, update changed writable columns.
8. On success, update persisted flags and dirty snapshots.
9. Return `Ok(self)`.

### 8.2 No Raising Bang Variants

Amber's convention is:

```amber
user.save.or_raise
```

Not:

```amber
user.save! # invalid design for ORM persistence
```

Allowed `!` methods are self-mutating and local:

```amber
assign!
clear_changes!
restore_attribute!
mark_persisted!
bind!
```

`bind!` is mutating class-level configuration, so the bang is appropriate even
though it returns a `Result`.

### 8.3 Executor Protocol

The ORM core talks to an executor object. A concrete adapter implements:

```amber
class Executor:
  def features()
  def schema_for(table)
  def insert(table, schema, attributes)
  def update(table, schema, id, changes)
  def delete(table, schema, id)
  def find(table, schema, id)
  def get(table, schema, where)
  def all(table, schema, limit: null, offset: null)
```

Return values:

```amber
schema_for -> Ok(Schema) | Err(SchemaError)
insert     -> Ok(row_map_or_attrs) | Err(PersistenceError)
update     -> Ok(row_map_or_attrs) | Err(PersistenceError / NotFoundError)
delete     -> Ok(DeleteResult) | Err(PersistenceError / NotFoundError)
find       -> Ok(row_map) | Err(NotFoundError / PersistenceError)
```

Adapter errors are wrapped so ORM callers can rescue at the ORM layer while the
original database error remains available:

```amber
Orm.PersistenceError(message, cause: sqlite_error)
```

### 8.4 Tableless Executor

`TablelessExecutor` implements the same protocol without DBMS calls.

Default behavior:

| Method | Behavior |
| --- | --- |
| `schema_for` | returns declared schema |
| `insert` | returns current attrs and marks persisted |
| `update` | returns current attrs |
| `delete` | marks destroyed |
| `find` | unsupported unless a custom tableless store is supplied |
| `get` / `all` | unsupported unless a custom tableless store is supplied |

### 8.5 Initial Query Relation

`where` returns an immutable `Query`; it does not execute immediately. `all`
and `first` are the execution boundary and return `Result`:

```amber
adults = User.where |query|:
  query.field(:age) >= 18

rows = adults.all.or_raise
row = adults.first.or_raise
```

Equality maps remain available for the common case:

```amber
ada = User.where({email: "ada@example.test"})
```

The comparison block supports `==`, `!=`, `>`, `>=`, `<`, `<=`, `one_of`,
`not_one_of`, `present?`, and `absent?`. Predicates compose explicitly with
`and_where` and `or_where`; repeated `where` calls compose with AND. Attribute
names are validated against the bound model schema before execution.

Adapters receive a small predicate tree (`WhereComparison`, `WhereJunction`, or
`WhereMap`). SQLite renders that tree with quoted identifiers and bound values.
Raw SQL strings are not accepted by this DSL. Ordering, projection, limits,
joins, and a larger relation algebra remain deferred.

### 8.6 Execution Instrumentation and Query Logging

The SQLite execution boundary is generic and sits around preparation plus
stepping. `sqlite3.Database#instrument_queries!` accepts an object with:

```amber
def instrument(execution, &execute)
```

`execution` contains `operation`, final SQL, and original bind parameters. The
instrumenter invokes `execute()` and returns its value. This around-call shape
is usable directly by tracing and OpenTelemetry integrations; logging is one
subscriber, not a special adapter branch.

ORM supplies `Instrumentation`, which creates one `ExecutionEvent`, calls
`started(event)` on subscribers, executes the query, records success or error,
then calls `finished(event)` in reverse subscriber order.

The standard query logger is deliberately opt-in:

```amber
instrumentation = orm.Instrumentation([
  orm.QueryLogger(dynamic: true, leave: true, refresh: 0.08)
])

User.bind!(orm_sqlite.adapter(db, instrumentation: instrumentation)).or_raise
```

With `dynamic: true`, the start row is written before execution and a no-GIL
spawned task refreshes monotonic elapsed time. A mutex protects the renderer;
parallel queries occupy stable rows in one ANSI live region. Completed rows end
with `v` or `x`; `leave: false` erases the batch. Query verbs have distinct
xterm colors (`SELECT`/schema cyan, `INSERT` green, `UPDATE` yellow, `DELETE`
red, transactions magenta). `dynamic: false` emits one ordinary final line and
does not use cursor controls.

This makes form/service models ergonomic:

```amber
form.save.or_raise
form.persisted? # true after successful validation
```

For stricter form objects, a tableless option can disable persisted state:

```amber
use orm.tableless(persisted: false):
  ...
```

## 9. SQLite Adapter

`orm.sqlite3.adapter(db)` wraps a `sqlite3.Database`.

```amber
sqlite3.memory() |db|:
  adapter = orm_sqlite.adapter(db)
  User.bind!(adapter).or_raise
```

### 9.1 Schema Introspection

For SQLite, schema introspection uses:

```sql
PRAGMA table_info("users")
```

or `PRAGMA table_xinfo` if hidden/generated columns need to be represented.
P0 can start with `table_info`.

Mapping:

| SQLite metadata | ORM column |
| --- | --- |
| `name` | `Column.name` |
| `type` | `Column.db_type` and logical `type` |
| `notnull` | `nullable: not notnull` |
| `pk` | `primary_key: pk > 0` |
| `dflt_value` | DB default text |

SQLite type normalization:

| SQLite declared type contains | ORM type |
| --- | --- |
| `INT` | `:integer` |
| `CHAR`, `CLOB`, `TEXT` | `:text` |
| `BLOB` or empty | `:binary` / `:any` depending policy |
| `REAL`, `FLOA`, `DOUB` | `:float` |
| other | `:any` |

SQLite generated primary key:

```sql
id integer primary key
```

should map to:

```amber
Column(:id, type: :integer, primary_key: true, generated: true)
```

### 9.2 Insert

For new records, insert writable non-generated columns:

```amber
db.insert(:users,
  columns: [:email, :age],
  values: [{email: "ada@example.test", age: 36}],
  returning: :all
)
```

If `RETURNING` is available, use it to refresh generated/default columns.
`sqlite3-amber` reports `returning: true` in `features()`, so P0 can rely on it
for the bundled SQLite adapter.

If a future adapter lacks returning, it should use `last_insert_id` plus a
follow-up `find`.

### 9.3 Update

Update only changed writable columns:

```amber
db.update(:users,
  set: {email: "new@example.test"},
  where: {id: 1},
  returning: :all
)
```

If `affected_rows == 0`, return `Err(NotFoundError)` or `Err(StaleRecordError)`
once optimistic locking exists. P0 can use `NotFoundError`.

### 9.4 Find/Get

`find(id)` uses the declared/inferred primary key:

```amber
db.get(:users, where: {id: id})
```

No primary key means `find`, `update`, and `delete` return
`Err(PersistenceError("model has no primary key"))` unless an explicit key
option is configured.

## 10. Error Surface

The surface is rescuable Amber failures stored inside `Err(...)`. Error identity
comes from ordinary class ancestry, so ORM errors retain rich fields such as
`ValidationError#errors` and `SchemaMismatchError#diff` while also matching the
universal `Exception` root.

Package source declares and exports local class names; import qualification
exposes them as `Orm.ValidationError`, `Orm.SchemaMismatchError`, and so on:

```amber
class OrmError < Exception
class SchemaError < OrmError
class SchemaMismatchError < SchemaError
class UnknownAttributeError < OrmError
class ValidationError < OrmError
class PersistenceError < OrmError
class NotFoundError < PersistenceError
class StaleRecordError < PersistenceError
```

Expected failure examples:

```amber
case user.save:
  when Ok(saved):
    saved
  when Err(Orm.ValidationError === e):
    e.errors
  when Err(e):
    raise e
```

Direct escalation:

```amber
user.save.or_raise
```

`Err(error).or_raise` re-raises the wrapped error through Amber's ordinary
exception path, so users can rescue:

```amber
try:
  user.save.or_raise
rescue Orm.ValidationError |e|:
  e.errors
```

Because these are ordinary `Exception` descendants, P0 keeps structured fields
directly on the raised value; no companion payload or registry-only substitute
is required.

## 11. Serialization and Params

P0 should expose explicit maps:

```amber
user.attributes
user.persisted_attributes
user.changed_attributes
user.to_map()
```

`attributes` returns current attribute values for all schema columns.

`to_map` should default to `attributes`, but later options can control hidden
fields:

```amber
user.to_map(only: [:id, :email])
user.to_map(except: [:password_hash])
```

No implicit JSON serialization policy should be added in P0. The ORM does not
know which fields are safe to export.

## 12. Naming and Namespace Notes

Package name choices:

| Candidate | Pros | Cons |
| --- | --- | --- |
| `orm` module, `amber-orm` repo | concise import, package name describes ecosystem | generic module id |
| `active_record` | clear inspiration | too Rails-specific, suggests more magic |
| `model` | simple | too broad |

Recommended:

```text
repo: amber-orm
module: orm
```

Error classes may render as `Orm.ValidationError` if Amber's package-to-constant
convention follows `sqlite3`'s `Sqlite3.Error` shape. The design uses `Orm.*`
for readability and `orm.*` for module functions.

## 13. P0 Feature Set

P0 should include:

- external package scaffold with root module `orm`;
- `Column`, `Schema`, and schema diffing;
- optional `expect_schema:` declarations with exact schema diffing;
- inferred schema binding for SQLite;
- attribute storage and generic access;
- generic attribute access for expected and inferred columns;
- dirty tracking;
- validation errors, one `validate:` block per model, and expression-based
  `expect` declarations with explicit code/message overrides;
- `Result`-returning `save`, `insert`, `update`, `delete`, `find`, `get`;
- tableless models;
- minimal lazy `where` queries with map and comparison predicates;
- SQLite adapter over `sqlite3-amber`;
- self-test covering tableless and SQLite-backed models.

P0 should not include:

- associations;
- migrations;
- callback chains;
- ordering, joins, projections, limits, and advanced relation algebra;
- eager loading;
- runtime accessor generation for inferred schemas;
- optimistic locking;
- transactions around multi-record operations;
- automatic JSON serialization;
- database-specific custom type registry.

## 14. Example Self-Test Shape

The self-test should exercise both modes.

```amber
package orm.selftest

import sqlite3
import orm
import orm.sqlite3 as orm_sqlite

export main

class User:
  use orm.model:
    expect_schema:
      column(:id, type: :integer, primary_key: true, generated: true)
      column(:email, type: :text, nullable: false)
      column(:age, type: :integer, nullable: true)

    validate:
      expect(:email, email.present?, message: "must be present")
      expect(:age, age.absent? or age >= 0,
        code: :gte, message: "must be greater than or equal to 0")

class SignupForm:
  use orm.tableless:
    schema:
      attribute(:email, type: :text)
    validate:
      expect(:email, email.present?, message: "must be present")

def main():
  score = 0

  form = SignupForm(email: "")
  if form.save.err?:
    score += 1

  sqlite3.memory |db|:
    db.exec("""
      create table users(
        id integer primary key,
        email text not null,
        age integer
      )
    """)

    User.bind!(orm_sqlite.adapter(db)).or_raise

    user = User(email: "ada@example.test", age: 36)
    user.save.or_raise
    if user.persisted? and user[:id] == 1:
      score += 1

    user[:email] = "ada@amber.test"
    if user.changed_attributes.contains?(:email):
      score += 1

    user.save.or_raise
    loaded = User.find(1).or_raise
    if loaded[:email] == "ada@amber.test":
      score += 1

  score
```

An inferred-schema test should separately verify:

```amber
class InferredUser:
  use orm.model(table: :users)

InferredUser.bind!(adapter).or_raise
u = InferredUser.find(1).or_raise
u[:email]
```

An explicit mismatch test should verify:

```amber
class BadUser:
  use orm.model(table: :users):
    expect_schema:
      column(:missing, type: :text)

BadUser.bind!(adapter).err? # true, SchemaMismatchError
```

## 15. Implementation Plan

### Phase 1 - Pure Core

Implement:

- `Column`;
- `Schema`;
- `SchemaDiff`;
- `AttributeSet`;
- `ChangeSet`;
- `ValidationErrors`;
- `TablelessExecutor`.

Tests:

- unknown attribute;
- assignment records changes;
- assigning original value clears change;
- validation collection;
- tableless save success/failure.

### Phase 2 - Model Runtime

Implement model mixin/base behavior:

- constructor initialization;
- class-level config;
- `bind!`;
- `save`;
- `find` / `get`;
- dirty state transitions.

Use a non-macro configuration form if needed to keep this phase unblocked.

### Phase 3 - Macro Surface

Implement:

- `use orm.model(...)`;
- `expect_schema` as an optional nested model macro;
- `column`;
- `attribute`;
- one `validate:` block per model;
- `expect` inside that block;
- generated explicit-column properties.

Keep macro output boring and inspectable: class methods, properties, and calls
to core helpers.

### Phase 4 - SQLite Adapter

Implement:

- `orm.sqlite3.adapter(db)`;
- schema introspection;
- insert/update/delete/find/get;
- adapter error wrapping.

Use `sqlite3-amber`'s public helpers:

- `db.select`;
- `db.get`;
- `db.insert`;
- `db.update`;
- `db.delete`;
- `sqlite3.ident` / safe identifier rendering where needed.

### Phase 5 - Glue Apps

Create examples:

- `examples/sqlite_basic.am`;
- `examples/tableless.am`;

The first glue app should prove:

1. expected schema bind succeeds;
2. mismatch returns `Err(SchemaMismatchError)`;
3. insert returns generated id;
4. update writes changed columns;
5. invalid save returns `Err(ValidationError)`;
6. tableless model validates and saves without DBMS.

### Phase 6 - Instrumentation

Implement:

- `sqlite3.SqlExecution` and `Database#instrument_queries!`;
- around-execution coverage for prepare, step, and transaction commands;
- `orm.Instrumentation` / `ExecutionEvent` subscribers;
- static and dynamic `QueryLogger` modes;
- concurrent live-row and `leave:` conformance tests.

### Native Coverage Boundary

The current `cpp-bytecode-direct-v1` backend restarts the whole program in the
VM after `NativeBailout`. Its eligibility allowlist must therefore remain
side-effect-free. ORM execution cannot truthfully report full native coverage
by adding opcodes to that allowlist: model code requires instance ivar loads and
stores, dynamic user-method dispatch, exception handlers, task/sync runtime
objects, and native-package calls. Once any store or query has happened, a
whole-program restart would duplicate observable effects.

`LOADSELF` can and should lower directly because `NativeFrame` already carries
the closure receiver. Full ORM coverage, however, requires a stateful native
lane that handles runtime slow paths and exceptions in place, without restarting
the bytecode program. Until that backend exists, the build must continue to
report `native_graph_full_coverage: false`; relabeling VM bridges as native is
not an acceptable implementation.

## 16. Open Questions

1. Should `expect_schema` reject extra DB columns by default, or should the
   default be "expected columns must match, DB extras ignored"?
2. Should `nullable: false` automatically add a validation rule, or should DB
   constraints and validations remain separate?
3. Should `Bool` map to SQLite `INTEGER` by default, and should `0/1` read back
   as `Bool` only when the model explicitly declares `type: :bool`?
4. Should tableless `save` mark records as persisted by default, or should form
   objects stay non-persisted unless configured?
5. What is the minimum macro support required for the P0 public surface, and do
   we need the non-macro fallback for bootstrapping?
6. Should `save(validate: false)` exist in P0, or is that escape hatch too early?
7. How much adapter feature discovery belongs in the ORM core vs individual
   adapter modules?
8. How large should the default inflection table be before users must spell
   `table:` explicitly?

## 17. Deferred Work

- Associations: `belongs_to`, `has_many`, `has_one`.
- Query relation additions: `order`, `limit`, projection, joins, lazy loading.
- Migrations and schema dumping.
- Optimistic locking.
- Transaction helpers around `save` and multi-model operations.
- Custom type registry.
- Model lifecycle callbacks beyond execution instrumentation.
- Secure serialization policies.
- Compile-time schema snapshots for generated accessors from inferred schemas.
- Adapter packages for PostgreSQL/MySQL once database packages exist.
