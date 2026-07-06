# DESIGN - `sqlite3` package public API

Status: proposed public-interface design  
Date: 2026-07-02  
Target: external Amber package `sqlite3`  
Scope: public API, resource semantics, error surface, SQL macro surface, and
full-native build contract  
Out of scope: implementation of the native thunks, ORM layer, custom SQL
callbacks, dynamic extension loading

## 0. Executive summary

`sqlite3` is a native-only Amber package for working with SQLite databases. It
has two public abstraction layers:

1. A core layer for connection handling, connection pooling, prepared
   statements, transactions, direct SQL execution, the exportable `sql"""..."""`
   macro, and sanitization helpers. This is the stable substrate for
   ActiveRecord-like packages.
2. A convenience layer for common query construction, safe identifier quoting,
   bulk inserts, insert-or-replace/upsert, simple select/update/delete builders,
   and SQLite JSON/JSONB helpers.

It exports a small module namespace:

```amber
import sqlite3
from sqlite3 import sql

threshold_age = 18

sqlite3.open("data/app.db") |db|:
  db.execute("create table if not exists users(id integer primary key, name text)")
  db.execute("insert into users(name) values (?)", params: ["Ada"])
  db.query(sql"""
    select id, name
    from users
    where age > #{threshold_age}
    order by id
  """)

  db.insert(:users,
    columns: [:name, :email],
    values: [
      ["Iris", "iris@example.test"],
      {name: "June", email: "june@example.test"}
    ]
  )
```

The single-connection surface is centered on two owned native resources:

- `Database`, wrapping a `sqlite3*`
- `Statement`, wrapping a `sqlite3_stmt*`

`Pool` is the shareable coordination object for Amber programs that use both
native threads and cooperative fibers. Individual `Database` and `Statement`
handles remain non-shareable.

Rows and command results are ordinary Amber values (`Map`, `List`, `Str`,
`Bytes`, `Int`, `Float`, `Bool`, `null`) so callers do not receive raw host
pointers or long-lived row handles.

The package has no bytecode implementation. A program that depends on this
package must be built as a full native executable, and the build must fail if
the compiler would embed VM fallback.

## 1. Design anchors

1. The Amber source is the public API. C is only the implementation of
   native-only leaves.
2. Database handles are explicit resources. Users close them with `close!` or
   use block-scoped forms that close automatically.
3. The core layer stays low-level enough for higher-level packages to build on.
4. The convenience layer is not an ORM. It builds safe SQL for common DML and
   query cases, then routes through the same core execution layer.
5. Parameter binding is the normal path. The `sql"""..."""` macro is the only
   interpolation path, and ordinary interpolants become bind parameters.
6. Query results use plain Amber values by default. A separate array-row path
   exists for duplicate column names and maximum predictability.
7. Errors are rescuable Amber errors, not integer return codes.
8. Full native is a package contract. No bytecode wrapper, whole-program restart,
   per-function VM bridge, or native fallback count is acceptable for consumer
   binaries.

### 1.1. Adapter substrate conventions

The core layer should be a model for other DBMS packages. Adapter packages are
expected to converge on these names and semantics where the database can support
them:

- connection object: `execute`, `execute_many`, `query`, `query_one`, `scalar`,
  `query_arrays`, `each`, `prepare`, and `transaction`;
- connection pool object: exclusive `checkout` / `with_connection` block forms,
  finite checkout timeouts, close/drain behavior, and lightweight stats;
- statement object: `bind!`, `execute`, `query`, `query_one`, `scalar`,
  `query_arrays`, `each`, metadata helpers, and explicit close/destroy;
- command result map with generic `affected_rows:` and `last_insert_id:` keys;
- result rows as ordinary `Map` values, with an array-row path for duplicate
  column labels;
- dialect-bound typed SQL values produced by an exportable `sql"""..."""`
  macro and explicit fragment/identifier helpers;
- feature discovery through common keys such as `adapter:`, `dialect:`,
  `placeholder_style:`, `returning:`, `savepoints:`, `nested_transactions:`,
  `pool:`, `pool_wait:`, and `pool_affinity:`.

Each DBMS package can add dialect-specific methods and feature keys, but the
portable substrate should remain small and predictable.

## 2. Package shape

The root module id is `sqlite3`.

Canonical imports:

```amber
import sqlite3

from sqlite3 import open, Database, Statement
from sqlite3 import pool, Pool
from sqlite3 import ConstraintError, BusyError
from sqlite3 import sql
```

Exports:

```amber
package sqlite3

export open, connect, memory, pool
export version, source_id, libversion_number, compile_options
export features, fragment, ident, quote_identifier, json, jsonb
export macro sql
export Database, Statement, Pool, PoolLease
export SqlFragment, SqlIdent, JsonExpr, JsonbExpr
export Error, OpenError, PrepareError, StepError, BindError, RangeError
export ConstraintError, BusyError, LockedError, ReadOnlyError
export InterruptError, CorruptError, MisuseError, TransactionError
export PoolError, PoolClosedError, PoolTimeoutError, PoolLeaseError
export JsonError, JsonUnavailableError, JsonbUnavailableError
```

The namespace object form is preferred for examples:

```amber
sqlite3.open("app.db")
rescue sqlite3.ConstraintError |e|:
  ...
```

The `sql` string tag is imported explicitly in examples because macros ride
Amber's ordinary import/export system and are not runtime values.

## 3. Full native contract

This package is native-only:

- `Database`, `Statement`, `Pool`, and `PoolLease` are
  `native class ... owned`.
- `open`, `connect`, `memory`, `pool`, and all methods on `Database`,
  `Statement`, `Pool`, and `PoolLease` are native-only leaves with no Amber
  fallback body.
- Bytecode execution fails closed with `NativeRequiredError` if a caller somehow
  reaches this package without a native build.
- The package distribution should vendor SQLite amalgamation sources by default
  for reproducible static links. A system `-lsqlite3` variant can be added later,
  but it is not the default public contract.
- Runtime dynamic extension loading is not exposed.

Consumer builds must use a full-native gate. With the current toolchain this is:

```sh
amberc build amber.build.json --target native --require-full-native
```

The build is acceptable only when the build summary reports:

```json
{
  "native_bytecode_fallback": false,
  "native_graph_full_coverage": true,
  "native_graph_vm_fallback_code_count": 0,
  "native_graph_fallback_code_count": 0
}
```

The eventual package manifest should make this automatic with a package-level
native policy, conceptually:

```toml
[native_policy]
require_full_native = true
allow_vm_fallback = false
```

Until that manifest field exists, package tests and consumer documentation must
pin `--require-full-native`.

## 4. Values and parameters

SQLite values map to Amber values as follows:

| SQLite storage class | Amber value |
| --- | --- |
| `NULL` | `null` |
| `INTEGER` | `Int` |
| `REAL` | `Float` |
| `TEXT` | `Str` |
| `BLOB` | `Bytes` |

Accepted bind values:

| Amber value | SQLite bind |
| --- | --- |
| `null` | `sqlite3_bind_null` |
| `Bool` | integer `0` or `1` |
| `Int` | `sqlite3_bind_int64` |
| `Float` | `sqlite3_bind_double` |
| `Str` | UTF-8 text |
| `Bytes` | blob |

`Symbol` values are not bound as SQL values in v1. Use `Str` explicitly.

Parameter containers:

- `List` or `Tuple` binds positional placeholders (`?`, `?NNN`) from 1.
- `Map` or `StrictMap` binds named placeholders (`:name`, `$name`, `@name`).
- Named parameter keys may be bare (`:id`, `"id"`) or exact (`":id"`,
  `"$id"`, `"@id"`). Bare keys are accepted only when they resolve
  unambiguously to one placeholder name.
- `null` means "no parameters".

## 5. Module functions

Conceptual signatures:

```amber
def open(
  path as Str,
  mode: :create,
  uri: false,
  busy_timeout: null,
  pragmas: {},
  &blk
) !{db, fs, ffi}  # Database without a block, block result with a block

def connect(..., &blk) !{db, fs, ffi}  # alias for open

def memory(
  name: null,
  shared: false,
  busy_timeout: null,
  pragmas: {},
  &blk
) !{db, ffi}  # Database without a block, block result with a block

def pool(
  path as Str,
  mode: :create,
  uri: false,
  min_idle_per_thread: 0,
  max_size: 5,
  max_size_per_thread: null,
  checkout_timeout: 5.seconds,
  idle_timeout: 60.seconds,
  max_lifetime: null,
  busy_timeout: null,
  pragmas: {},
  &blk
) !{db, fs, ffi}  # Pool without a block, block result with a block

def version() -> Str !{ffi}
def source_id() -> Str !{ffi}
def libversion_number() -> Int !{ffi}
def compile_options() -> List !{ffi}
def features() -> Map !{ffi}

string_tag macro def sql(t as Ast.StringTemplate) -> Ast
def fragment(text as Str, params as List = []) -> SqlFragment
def ident(name) -> SqlIdent           # validated SQL identifier marker
def quote_identifier(name) -> Str      # quoted SQLite identifier spelling
def json(value) -> JsonExpr            # query-builder JSON text expression
def jsonb(value) -> JsonbExpr          # query-builder JSONB expression
```

`open` and `memory` return `Database` when called without a block. With a block,
they pass the opened database to the block, close it in an `ensure` path, and
return the block result.

`pool` returns `Pool` when called without a block. With a block, it passes the
pool to the block, closes the pool in an `ensure` path, and returns the block
result. Pools open connections lazily on checkout.

Pool sizing:

- `min_idle_per_thread:` keeps at least that many idle connections for each
  native thread that has used the pool, subject to `max_size:`.
- `max_size:` is the global open connection cap.
- `max_size_per_thread:` optionally caps open connections owned by one native
  thread. `null` means "bounded only by `max_size:`".
- `checkout_timeout:` is the default maximum wait for an available connection.
- `idle_timeout:` is the maximum idle lifetime before a connection may be
  pruned.
- `max_lifetime:` is the maximum connection age before a connection is retired
  after release. `null` means no age-based retirement.

Open modes:

| Mode | SQLite flags | Meaning |
| --- | --- | --- |
| `:readonly` | `READONLY` | existing database, read only |
| `:readwrite` | `READWRITE` | existing database, read/write |
| `:create` | `READWRITE | CREATE` | create if missing, default |

`busy_timeout:` accepts `null`, a numeric seconds value, or `TimePeriod`.
Negative and non-finite values raise `ArgumentError`.

`pragmas:` is a map of simple startup pragmas applied immediately after open and
before the block body is called. Keys must be symbols or strings matching a safe
SQLite identifier. Values may be `Bool`, `Int`, `Float`, `Str`, `Symbol`, or
`null`.

`features()` returns library-level feature information for the linked SQLite
build:

```amber
{
  adapter: :sqlite3,
  dialect: :sqlite,
  version: "3.53.3",
  source_id: "...",
  libversion_number: 3053003,
  compile_options: ["..."],
  placeholder_style: :qmark,
  named_parameters: true,
  returning: true,
  savepoints: true,
  nested_transactions: :savepoint,
  ddl_transactions: true,
  pool: true,
  pool_wait: :cooperative,
  pool_affinity: :native_thread,
  json: true,
  jsonb: true,
  jsonb_each: true,
  jsonb_tree: true
}
```

The package must probe the actual linked SQLite build rather than hard-coding
versions. JSON functions can be omitted with `SQLITE_OMIT_JSON`, JSONB storage
functions require a build that provides `jsonb()`, and the table-valued
`jsonb_each()` / `jsonb_tree()` helpers are separate capabilities.

The generic keys (`adapter:`, `dialect:`, `placeholder_style:`, `returning:`,
`savepoints:`, `nested_transactions:`, `ddl_transactions:`, `pool:`,
`pool_wait:`, and `pool_affinity:`) are intended as the common feature vocabulary
for future `postgres`, `mysql`, and other DBMS packages. SQLite-only diagnostics
such as `source_id:`, `libversion_number:`, and `compile_options:` remain in the
same map for support and reproducibility.

### 5.1. SQL macro and sanitization helpers

The basic layer exports `sql` as a string-tag macro using Amber's macro and
multiline string import rules:

```amber
from sqlite3 import sql

threshold_age = 18

rows = db.query(sql"""
  select id, name
  from users
  where age > #{threshold_age}
""")
```

`SqlFragment`, `SqlIdent`, `JsonExpr`, and `JsonbExpr` are opaque public value
types. Callers create them through `sql"""..."""`, `sqlite3.fragment(...)`,
`sqlite3.ident(...)`, `sqlite3.json(...)`, and `sqlite3.jsonb(...)`; higher-level
packages can accept and forward them without depending on private fields.

These typed values are dialect-bound. `sqlite3` methods accept fragments and
identifiers produced by `sqlite3` only. A fragment from another package, such as
`postgres.sql"""..."""`, raises `TypeError` if passed to this adapter. That
keeps placeholder syntax, identifier quoting, and JSON expression rules from
crossing dialect boundaries accidentally.

`sql"""..."""` expands to a typed SQL value that carries SQL text plus bind
parameters. It is accepted by core direct-execution methods and by
convenience-builder slots that need SQL expressions, such as `where:`,
`where_not:`, `columns:`, `group_by:`, `having:`, `order:`, and `returning:`.

Interpolation is not string concatenation:

- `#{value}` becomes a bound SQLite parameter.
- List interpolation is allowed only in value-list contexts accepted by the
  macro, such as `IN (#{ids})`. A statically empty list is a compile-time
  diagnostic; a runtime empty list raises `ArgumentError`.
- SQL identifiers must be explicit. Bare interpolation in table, column, order,
  collation, pragma-name, or other identifier positions is rejected.

Dynamic identifiers use `sqlite3.ident(...)`:

```amber
table_name = :users
column_name = :age

rows = db.query(sql"""
  select *
  from #{sqlite3.ident(table_name)}
  where #{sqlite3.ident(column_name)} > #{threshold_age}
""")
```

`sqlite3.ident(name)` validates one identifier or a dotted identifier path and
returns a typed identifier marker for the macro and builders. It accepts
`Symbol`, `Str`, or a `List`/`Tuple` of identifier segments such as
`[:main, :users]`.

`sqlite3.quote_identifier(name)` returns the quoted SQLite spelling for code
generators, logging, and debugging. It does not create a SQL fragment by itself.

`sqlite3.fragment(text, params as List = [])` creates a trusted raw SQL fragment
for cases where SQL syntax is already built by another safe compiler. It uses a
second positional argument, not a `params:` keyword, because the API has no
other fragment options. Prefer `sql"""..."""` for hand-written SQL. Values still
belong in bind parameters, not in escaped strings. Fragment parameters are
positional; fragment text should use SQLite `?` or `?NNN` placeholders, not
named placeholders.

## 6. `Database`

Conceptual surface:

```amber
native class Database from "sqlite3.Database" owned:
  def close!()                         from "sqlite3.database_close"
  def destroy!()                       from "sqlite3.database_destroy"
  def closed?() -> Bool                from "sqlite3.database_closed"

  def filename(schema: :main) -> Str   from "sqlite3.database_filename"
  def readonly?(schema: :main) -> Bool from "sqlite3.database_readonly"
  def in_transaction?() -> Bool        from "sqlite3.database_in_transaction"

  def busy_timeout!(timeout) -> self   from "sqlite3.database_busy_timeout"
  def interrupt!()                     from "sqlite3.database_interrupt"

  def exec(sql) -> Map                 from "sqlite3.database_exec"

  def execute(
    sql,
    params: null
  ) -> Map                             from "sqlite3.database_execute"

  def execute_many(
    sql,
    params as List:
  ) -> Map                             from "sqlite3.database_execute_many"

  def query(
    sql,
    params: null
  ) -> List                            from "sqlite3.database_query"

  def query_one(
    sql,
    params: null,
    default: null
  )                                   from "sqlite3.database_query_one"

  def scalar(
    sql,
    params: null,
    default: null
  )                                   from "sqlite3.database_scalar"

  def query_arrays(
    sql,
    params: null
  ) -> Map                             from "sqlite3.database_query_arrays"

  def each(sql, params: null, &blk) -> Int
                                       from "sqlite3.database_each"

  def each_array(sql, params: null, &blk) -> Int
                                       from "sqlite3.database_each_array"

  def prepare(sql, &blk)               from "sqlite3.database_prepare"

  def begin!(mode: :deferred) -> self  from "sqlite3.database_begin"
  def commit!() -> self                from "sqlite3.database_commit"
  def rollback!() -> self              from "sqlite3.database_rollback"

  def transaction(mode: :deferred, &blk)
                                       from "sqlite3.database_transaction"

  def savepoint(name: null, &blk)      from "sqlite3.database_savepoint"

  def pragma(name)                     from "sqlite3.database_pragma_get"
  def pragma!(name, value) -> self     from "sqlite3.database_pragma_set"

  def changes() -> Int                 from "sqlite3.database_changes"
  def total_changes() -> Int           from "sqlite3.database_total_changes"
  def last_insert_rowid() -> Int       from "sqlite3.database_last_insert_rowid"

  def features() -> Map                from "sqlite3.database_features"
  def json_available?() -> Bool        from "sqlite3.database_json_available"
  def jsonb_available?() -> Bool       from "sqlite3.database_jsonb_available"
  def jsonb_each_available?() -> Bool  from "sqlite3.database_jsonb_each_available"
  def jsonb_tree_available?() -> Bool  from "sqlite3.database_jsonb_tree_available"
  def to_json(value) -> Str            from "sqlite3.database_to_json"
  def to_jsonb(value) -> Bytes         from "sqlite3.database_to_jsonb"

  def insert(table, columns:, values:, returning: null)
                                       from "sqlite3.database_insert"
  def insert_or_ignore(table, columns:, values:, returning: null)
                                       from "sqlite3.database_insert_or_ignore"
  def insert_or_replace(table, columns:, values:, returning: null)
                                       from "sqlite3.database_insert_or_replace"
  def upsert(table, columns:, values:, returning: null)
                                       from "sqlite3.database_insert_or_replace"
  def update(table, set:, where: null, where_not: null, returning: null)
                                       from "sqlite3.database_update"
  def delete(table, where: null, where_not: null, returning: null)
                                       from "sqlite3.database_delete"
  def select(
    table,
    columns: :all,
    where: null,
    where_not: null,
    group_by: null,
    having: null,
    order: null,
    limit: null,
    offset: null
  ) -> List                            from "sqlite3.database_select"
  def get(
    table,
    columns: :all,
    where: null,
    where_not: null,
    group_by: null,
    having: null,
    order: null
  )                                   from "sqlite3.database_get"
```

### 6.1. Command result

`exec`, `execute`, and `execute_many` return an ordinary map:

```amber
{
  affected_rows: 1,
  changes: 1,
  total_changes: 9,
  last_insert_id: 42,
  last_insert_rowid: 42
}
```

`affected_rows:` and `last_insert_id:` are the generic adapter keys intended for
higher-level packages. In SQLite, `affected_rows:` is the same value as
`changes:`, and `last_insert_id:` is the same value as `last_insert_rowid:`.
SQLite-specific keys remain available for callers that need exact SQLite
semantics.

Core SQL arguments accept either a `Str` or a typed SQL value produced by
`sql"""..."""` or `sqlite3.fragment(...)`.

`exec(sql)` is for trusted schema or migration scripts and accepts no separate
parameters. It may run more than one statement. A typed SQL value passed to
`exec` must not carry bind parameters.

`execute(sql, params:)` is for one parameterized statement. It runs the
statement to completion and raises `StepError` if the statement unexpectedly
produces rows. Use `query`, `query_one`, or `scalar` for row-producing SQL.
When `sql` is a typed SQL value that already carries parameters, `params:` must
be `null`.

`execute_many(sql, params:)` runs the same statement for each parameter entry in
the given list. It should be implemented as one prepared statement reset between
entries. Typed SQL values are allowed only when they do not carry their own
parameters; each entry in `params:` supplies all binds for that iteration.
`execute_many` does not open a transaction automatically. Callers should wrap it
in `transaction` when they need atomic bulk writes.

`prepare(sql)` accepts `Str` or parameterless typed SQL. A typed SQL value that
already carries parameters raises `ArgumentError`; reusable prepared statements
should use placeholders and pass values to `Statement#execute`, `Statement#query`,
or `Statement#bind!`.

`execute`, `query`, `query_one`, `scalar`, `query_arrays`, `each`, `each_array`,
and `prepare` must reject trailing non-whitespace SQL after the first prepared
statement. Use `exec` for trusted multi-statement scripts.

### 6.2. Query result shapes

`query` returns a list of ordinary maps:

```amber
rows = db.query("select id, name from users")
rows[0][:id]
rows[0]["name"]
```

The row map is name-indifferent, matching ordinary Amber `Map` behavior. Column
labels are taken from SQLite's column names. If a result set contains duplicate
column labels, later columns overwrite earlier columns in the map. Call
`query_arrays` when duplicate labels matter.

`query_arrays` returns:

```amber
{
  columns: ["id", "name"],
  rows: [[1, "Ada"], [2, "Iris"]]
}
```

`query_one(sql, default:)` returns the first row map. If the statement produces
no rows, it returns `default`. `scalar(sql, default:)` returns the first column
of the first row; if there is no row, it returns `default`. A row whose first
column is SQL `NULL` returns `null`, not `default`.

`each` and `each_array` stream rows through a block and return the number of rows
yielded. The block callback is part of the full-native contract: it must call a
native-compiled Amber closure, not re-enter bytecode VM fallback.

### 6.3. Transactions

Transaction block form is the canonical form:

```amber
db.transaction(mode: :immediate) |tx|:
  tx.execute("insert into users(name) values (?)", params: ["Ada"])
  tx.execute("insert into users(name) values (?)", params: ["Iris"])
```

Allowed modes:

- `:deferred`
- `:immediate`
- `:exclusive`

The block result is returned. On normal block return, the transaction commits.
On exception, it rolls back and re-raises the original exception.

Nested `transaction` calls use generated savepoints. Explicit `begin!` inside an
active transaction raises `TransactionError`; use `transaction` or `savepoint`
for nesting.

When `transaction(mode:)` is called inside an active transaction, only
`mode: :deferred` is accepted. A nested `mode: :immediate` or `mode: :exclusive`
raises `TransactionError`, because SQLite savepoints do not upgrade the outer
transaction mode.

Generated savepoint names are package-private. `savepoint(name:)` is for
advanced callers that need stable names for debugging; names must validate as
safe SQLite identifiers.

## 7. `Pool`

`Pool` is part of the basic connection layer. It is the only public object in
this package intended to be shared across Amber native threads and cooperative
fibers.

Conceptual surface:

```amber
native class Pool from "sqlite3.Pool" owned:
  def close!() -> null                 from "sqlite3.pool_close"
  def destroy!() -> null               from "sqlite3.pool_destroy"
  def closed?() -> Bool                from "sqlite3.pool_closed"

  def checkout(timeout: null, &blk)    from "sqlite3.pool_checkout"
  def with_connection(timeout: null, &blk)
                                       from "sqlite3.pool_checkout"
  def acquire(timeout: null) -> PoolLease
                                       from "sqlite3.pool_acquire"

  def stats() -> Map                   from "sqlite3.pool_stats"
  def prune!() -> Map                  from "sqlite3.pool_prune"
  def disconnect!() -> Map             from "sqlite3.pool_disconnect"

native class PoolLease from "sqlite3.PoolLease" owned:
  def db() -> Database                 from "sqlite3.pool_lease_db"
  def release!() -> null               from "sqlite3.pool_lease_release"
  def discard!() -> null               from "sqlite3.pool_lease_discard"
  def released?() -> Bool              from "sqlite3.pool_lease_released"
```

Checkout forms:

```amber
sqlite3.pool("app.db", max_size: 10) |pool|:
  pool.checkout |db|:
    db.query("select id, name from users")

lease = pool.acquire(timeout: 2.seconds)
try:
  lease.db().execute("insert into log(message) values (?)", params: ["started"])
ensure:
  lease.release!()
```

`checkout` and `with_connection` require a block and return the block result.
Calling either without a block raises `ArgumentError`. Use `acquire` for the
manual lease form that returns a `PoolLease`.

Thread and fiber rules:

- `Pool` is shareable across native threads and cooperative fibers.
- `Database`, `Statement`, and `PoolLease` are not shareable.
- A physical SQLite connection is owned by one native thread for its lifetime.
  The pool keeps per-thread idle lists and never checks out a connection created
  on one native thread to another native thread.
- A checkout grants exclusive use to the current fiber/task. Sibling fibers on
  the same native thread may share the pool, but they must check out separate
  connections.
- A lease is a runtime pinning region. If a fiber suspends while holding a
  lease, the runtime must resume that fiber on the same native thread until the
  lease is released. If pinning is unavailable, suspension while holding a lease
  raises `PoolLeaseError`.
- A `Database`, `Statement`, or `PoolLease` cannot be sent through cross-thread
  channels, captured by `task.spawn`, or otherwise cross Amber's shareability
  boundary. Violations raise the runtime's shareability error before any SQLite
  call is attempted.

Pool waiting:

- Waiting for a connection observes `checkout_timeout:` or the per-call
  `timeout:`.
- From a cooperative fiber, pool wait parks the current fiber and lets the
  native thread run other work.
- From plain native-thread code, pool wait blocks that native thread on a native
  condition variable.
- Timeout raises `PoolTimeoutError`.
- Closing the pool wakes waiters with `PoolClosedError`.

Lease release:

- Block checkout releases in an `ensure` path.
- `release!` returns a healthy connection to the owning thread's idle list.
- `discard!` closes the connection instead of returning it to the pool.
- If a connection is closed manually while leased, the lease is discarded on
  release.
- On release, the pool rolls back any active transaction and closes open child
  statements before reuse. If cleanup fails, the connection is discarded.
- `release!` and `discard!` are idempotent.

`stats()` returns an ordinary map:

```amber
{
  min_idle_per_thread: 0,
  max_size: 10,
  max_size_per_thread: 2,
  open: 4,
  idle: 2,
  leased: 2,
  waiting: 1,
  closed: false
}
```

`prune!` closes expired idle connections and returns a command-result-like map
with at least `closed:`. `disconnect!` closes all idle connections and marks
leased connections to be discarded when their leases are released.

## 8. `Statement`

Conceptual surface:

```amber
native class Statement from "sqlite3.Statement" owned:
  def close!() -> null                 from "sqlite3.statement_close"
  def destroy!() -> null               from "sqlite3.statement_destroy"
  def closed?() -> Bool                from "sqlite3.statement_closed"

  def sql() -> Str                     from "sqlite3.statement_sql"
  def column_count() -> Int            from "sqlite3.statement_column_count"
  def column_names() -> List           from "sqlite3.statement_column_names"
  def columns() -> List                from "sqlite3.statement_columns"
  def parameter_count() -> Int         from "sqlite3.statement_parameter_count"
  def parameter_names() -> List        from "sqlite3.statement_parameter_names"
  def readonly?() -> Bool              from "sqlite3.statement_readonly"
  def busy?() -> Bool                  from "sqlite3.statement_busy"

  def bind!(params = null) -> self     from "sqlite3.statement_bind"
  def reset!() -> self                 from "sqlite3.statement_reset"
  def clear_bindings!() -> self        from "sqlite3.statement_clear_bindings"

  def step()                           from "sqlite3.statement_step"
  def step_array()                     from "sqlite3.statement_step_array"

  def execute(params: null) -> Map     from "sqlite3.statement_execute"
  def query(params: null) -> List      from "sqlite3.statement_query"
  def query_one(params: null, default: null)
                                       from "sqlite3.statement_query_one"
  def scalar(params: null, default: null)
                                       from "sqlite3.statement_scalar"
  def query_arrays(params: null) -> Map
                                       from "sqlite3.statement_query_arrays"

  def each(params: null, &blk) -> Int  from "sqlite3.statement_each"
  def each_array(params: null, &blk) -> Int
                                       from "sqlite3.statement_each_array"
```

Low-level stepping:

- `bind!` binds parameters without stepping.
- `step` returns the next row map, or `null` when the statement is done.
- `step_array` returns the next row values list, or `null` when done.
- `reset!` returns the statement to the start.
- `clear_bindings!` clears all bound values.

Statement metadata:

- `column_names()` returns result column labels.
- `columns()` returns metadata maps with at least `name:`. When the linked build
  provides column metadata, entries may also include `database:`, `table:`,
  `origin_name:`, and `declared_type:`.
- `parameter_names()` returns one entry per bind slot. Positional slots are
  `null`; named slots include their SQLite prefix, such as `":id"`.

High-level helpers (`execute`, `query`, `query_one`, `scalar`, `query_arrays`,
`each`, and `each_array`) clear old bindings, bind new parameters, run to
completion, and reset the statement before returning.

`Database#prepare(sql) |stmt|:` finalizes the statement in an `ensure` path.
Without a block, `prepare(sql)` returns an open `Statement` owned by the caller.

## 9. Convenience query layer

The convenience layer is deliberately above prepared statements but below an
ORM. It builds SQL for common operations, quotes identifiers, flattens values
into bind parameters, then delegates to `execute`, `query`, or `query_one`.

It is intended for:

- application code that wants readable database calls without a model layer;
- higher-level packages that need a safe SQL construction substrate;
- code generators that want deterministic SQL strings and parameter lists.

It is not intended to hide SQL or infer schema.

### 9.1. Identifier and fragment rules

Identifier arguments accept `Symbol`, `Str`, or `SqlIdent`:

```amber
:users
"users"
[:users, :email]
sqlite3.ident([:main, :users])
```

Both symbols and strings are treated as identifiers and quoted by the builder.
`SqlIdent` values are already validated identifier markers. A string is not raw
SQL. For hand-written SQL expressions, predicates, or aggregate projections,
prefer the exported `sql` macro:

```amber
from sqlite3 import sql

sql"""count(*) as count"""
sql"""created_at > #{cutoff}"""
```

For SQL produced by another trusted compiler, use
`sqlite3.fragment(text, params)`:

```amber
sqlite3.fragment("count(*) as count")
sqlite3.fragment("created_at > ?", [cutoff])
```

Macro fragments and trusted fragments carry their own bind parameters and are
never produced implicitly from strings. Dynamic identifiers inside macro SQL
must use `sqlite3.ident(...)`; values use normal interpolation and become bind
parameters.

### 9.2. Insert and insert-or-replace

Canonical forms:

```amber
db.insert(:users,
  columns: [:name, :email],
  values: [
    ["Ada", "ada@example.test"],
    ("Iris", "iris@example.test"),
    {name: "June", email: "june@example.test"}
  ]
)

db.insert_or_replace(:users,
  columns: [:id, :name, :email],
  values: [
    {id: 1, name: "Ada", email: "ada@example.test"}
  ]
)

db.upsert(:users,
  columns: [:id, :name],
  values: [[1, "Ada"]]
)
```

`upsert` is an exact alias for `insert_or_replace` in v1. This is SQLite's
`INSERT OR REPLACE` behavior, including its replace/delete semantics. A future
API can add a separate SQL-standard `ON CONFLICT DO UPDATE` helper without
changing this alias.

Because `upsert` is an alias for SQLite replacement semantics, higher-level
cross-DB packages should not use it as the portable upsert primitive. They
should either call `insert_or_replace` deliberately on SQLite or wait for a
separate conflict-update helper with explicit conflict-target and update-set
semantics.

`values:` accepts either a single row or a list of rows. A row can be:

- `List` or `Tuple`, whose size must equal `columns.count()`;
- `Map` or `StrictMap`, whose keys must exactly match the requested columns;
- a mix of row shapes in the same bulk insert.

If `columns:` is omitted in a future version, map rows may be allowed to infer
columns. In v1, `columns:` is required for deterministic SQL and predictable
array/tuple rows.

Return value:

- without `returning:`, insert helpers return the command result map;
- with `returning: :all` or `returning: [:id, ...]`, they return `List[Map]`;
- `returning:` values are identifiers unless wrapped in `sql"""..."""` or
  `sqlite3.fragment(...)`.

### 9.3. Update, delete, select, and get

Canonical forms:

```amber
db.update(:users,
  set: {name: "Ada Lovelace"},
  where: {id: 1}
)

db.delete(:users, where: {id: 1})

db.select(:users,
  columns: [:id, :name],
  where: {active: true},
  where_not: {role: "admin"},
  order: [[:name, :asc]],
  limit: 20
)

db.get(:users, where: {id: 1})

db.select(:events,
  columns: [:kind, sql"""count(*) as count"""],
  group_by: [:kind],
  having: sql"""count(*) > #{minimum_count}"""
)
```

`columns:` accepts:

- `:all`, meaning `*`;
- a single identifier;
- a list of identifiers;
- a `sql"""..."""` macro fragment;
- an explicit `sqlite3.fragment(...)` value;
- a list mixing identifiers, macro fragments, and trusted fragments.

`where:` accepts:

- `null`, meaning no predicate;
- a map of equality predicates joined with `AND`;
- a `sql"""..."""` macro fragment;
- an explicit `sqlite3.fragment(...)` value.

Predicate map keys are identifiers. Predicate map values render as:

- `null`: `"column" IS NULL`;
- scalar value: `"column" = ?`;
- `List` or `Tuple`: `"column" IN (?, ...)`;
- empty `List` or `Tuple`: a constant false predicate before any `where_not:`
  group negation.

`where_not:` accepts the same shapes as `where:`. It is rendered as
`NOT (<predicate>)`, and is combined with `where:` using `AND` when both are
present. A `where_not:` map is first rendered as normal equality predicates and
then negated as a group.

Map values and fragment parameters are appended in SQL rendering order:
`where:`, then `where_not:`, then `having:`.

`group_by:` accepts:

- `null`, meaning no grouping;
- a single identifier;
- a list of identifiers;
- a `sql"""..."""` macro fragment;
- an explicit `sqlite3.fragment(...)` value;
- a list mixing identifiers, macro fragments, and trusted fragments.

`having:` accepts:

- `null`, meaning no aggregate predicate;
- a map of equality predicates joined with `AND`;
- a `sql"""..."""` macro fragment;
- an explicit `sqlite3.fragment(...)` value.

`having:` map values use the same rendering rules as `where:` map values.

`having:` without `group_by:` is allowed because SQLite accepts aggregate
queries with no explicit grouping key.

`order:` accepts:

- a single identifier;
- a list of identifiers;
- a list of `[identifier, :asc | :desc]` pairs;
- a `sql"""..."""` macro fragment;
- an explicit `sqlite3.fragment(...)` value.

`limit:` and `offset:` must be non-negative `Int` values.

`select` returns `List[Map]`. `get` returns one map or `null`.
`get` renders the same SQL shape as `select` with an implicit `LIMIT 1`.
Without `order:`, which row is returned is SQLite's ordinary query-plan result
and should not be treated as stable.

`update` and `delete` return the command result map by default. With
`returning:`, they return `List[Map]`.

### 9.4. JSON and JSONB support

The package exposes JSON helpers in two forms:

```amber
sqlite3.json(value)
sqlite3.jsonb(value)

db.to_json(value)
db.to_jsonb(value)
```

`sqlite3.json(value)` and `sqlite3.jsonb(value)` are query-builder expression
wrappers. In insert/update values they render as `json(?)` or `jsonb(?)`, with
the Amber value serialized to canonical JSON and bound as text:

```amber
db.insert(:events,
  columns: [:kind, :payload],
  values: [
    ["created", sqlite3.jsonb({id: 1, name: "Ada"})]
  ]
)
```

`db.to_json(value)` returns canonical JSON text using the linked SQLite JSON
implementation and raises `JsonUnavailableError` when the linked build does not
provide JSON support. `db.to_jsonb(value)` returns SQLite JSONB bytes and raises
`JsonbUnavailableError` when the linked build does not provide JSONB.

Feature probes:

```amber
db.json_available?()
db.jsonb_available?()
db.jsonb_each_available?()
db.jsonb_tree_available?()
db.features()
```

The package must probe actual function availability on the open connection.
Important SQLite version facts as of the design date:

- JSON functions are built in by default from SQLite 3.38.0, unless omitted by
  `SQLITE_OMIT_JSON`.
- JSONB storage/functions begin with SQLite 3.45.0.
- `jsonb_each()` and `jsonb_tree()` begin with SQLite 3.51.0.

The convenience layer must not silently downgrade JSON expressions. Rendering
`sqlite3.json(value)` raises `JsonUnavailableError` when JSON is unavailable.
Rendering `sqlite3.jsonb(value)` raises `JsonbUnavailableError` when JSONB is
unavailable. Callers that want portability can branch on `db.json_available?()`
or `db.jsonb_available?()` and choose the expression themselves.

JSON wrapper inputs must be JSON-compatible Amber values: `null`, `Bool`, `Int`,
`Float`, `Str`, `List`, `Tuple`, `Map`, or `StrictMap` with string or symbol
keys. Unsupported values raise `TypeError`.

## 10. Lifecycle and ownership

`Database`, `Statement`, `Pool`, and `PoolLease` are owned native handles.

Rules:

- `close!` is idempotent.
- `destroy!` is the native-class destructor hook and behaves like `close!`.
- Using a closed handle raises `LifetimeError` or a package-specific subclass
  when the operation can identify one.
- `Pool` is shareable across Amber native threads and cooperative fibers.
- `Database`, `Statement`, and `PoolLease` are thread/fiber confined. They may
  be used only by the execution context that opened or checked them out.
- A checked-out `Database` may be used by one fiber at a time. It must not be
  shared with sibling fibers, even on the same native thread.
- A `Database` tracks statements prepared from it. Closing a database closes any
  still-open statements before closing the SQLite handle.
- A `Statement` cannot outlive its database. If the database is closed, all child
  statements become closed.
- A `PoolLease` keeps enough pool state alive to release safely. If the pool is
  closed while leases are outstanding, those leases may finish, but their
  connections are discarded rather than returned.

The scoped forms are preferred:

```amber
sqlite3.open("app.db") |db|:
  db.prepare("insert into log(message) values (?)") |stmt|:
    stmt.execute(params: ["started"])
```

## 11. Capabilities and effects

The package needs native build trust:

```toml
[capabilities]
ffi = true
```

Opening a database also checks host resource capabilities at runtime.

For `sqlite3.open(path, mode: :readonly)`:

- `db.connect` for the database target
- `fs.read` for the database path

For `:readwrite` and `:create`:

- `db.connect` for the database target
- `fs.read` for the database path or containing directory
- `fs.write` for the containing directory or prefix, because SQLite may create
  journal, WAL, and shared-memory sidecar files

For `sqlite3.memory(...)`:

- `db.connect` for `:memory:` or the shared memory name
- no filesystem grant

For `sqlite3.pool(...)`:

- the same grants as `sqlite3.open(...)` for the configured target;
- grants are checked when the pool is created and reused for lazy connection
  opens inside that pool;
- connections open lazily on checkout. `min_idle_per_thread:` may warm or retain
  idle connections only for native threads that have already used the pool.

`uri: true` does not bypass resource checks. The package parses the URI enough
to identify the database target and rejects URI forms whose filesystem behavior
cannot be checked conservatively.

Raw SQL must not bypass filesystem capabilities. In v1, SQL-level `ATTACH` and
`VACUUM INTO` are rejected by the native layer. A future package can expose
explicit `attach(path, ...)` or `vacuum_into(path, ...)` APIs with normal
resource checks.

Effects:

- `open` is conservatively `!{db, fs, ffi}`.
- `pool` is conservatively `!{db, fs, ffi}`.
- `memory` is `!{db, ffi}`.
- operations on an already-open handle are `!{db, ffi}`.
- `Pool#checkout` and `Pool#acquire` are `!{db, ffi, task}` because waiting can
  park the current fiber. If called outside the fiber scheduler, waiting blocks
  the current native thread instead.

## 12. Error surface

Package error hierarchy:

```text
Sqlite3.Error < NativeError
Sqlite3.OpenError < Sqlite3.Error
Sqlite3.PrepareError < Sqlite3.Error
Sqlite3.BindError < Sqlite3.Error
Sqlite3.StepError < Sqlite3.Error
Sqlite3.RangeError < Sqlite3.BindError
Sqlite3.ConstraintError < Sqlite3.StepError
Sqlite3.BusyError < Sqlite3.StepError
Sqlite3.LockedError < Sqlite3.StepError
Sqlite3.ReadOnlyError < Sqlite3.StepError
Sqlite3.InterruptError < Sqlite3.StepError
Sqlite3.CorruptError < Sqlite3.StepError
Sqlite3.MisuseError < Sqlite3.Error
Sqlite3.TransactionError < Sqlite3.Error
Sqlite3.PoolError < Sqlite3.Error
Sqlite3.PoolClosedError < Sqlite3.PoolError
Sqlite3.PoolTimeoutError < Sqlite3.PoolError
Sqlite3.PoolLeaseError < Sqlite3.PoolError
Sqlite3.JsonError < Sqlite3.Error
Sqlite3.JsonUnavailableError < Sqlite3.JsonError
Sqlite3.JsonbUnavailableError < Sqlite3.JsonError
```

Exported aliases expose those classes as:

```amber
rescue sqlite3.ConstraintError |e|:
  ...

from sqlite3 import ConstraintError
rescue ConstraintError |e|:
  ...
```

General argument and host errors keep existing Amber classes:

- invalid `mode:`, invalid parameter shape, invalid pragma name:
  `ArgumentError`
- unsupported bind value: `TypeError`
- unsupported JSON wrapper value: `TypeError`
- missing resource grant: `CapabilityError`
- bytecode-only use: `NativeRequiredError`
- closed handle: `LifetimeError`
- checkout timeout: `PoolTimeoutError`
- checkout or acquire after pool close: `PoolClosedError`
- using a lease from the wrong execution context: `PoolLeaseError` or the
  runtime shareability error, depending on where the violation is detected

SQLite result-code mapping:

| SQLite result | Amber error |
| --- | --- |
| `SQLITE_BUSY` | `BusyError` |
| `SQLITE_LOCKED` | `LockedError` |
| `SQLITE_CONSTRAINT*` | `ConstraintError` |
| `SQLITE_READONLY*` | `ReadOnlyError` |
| `SQLITE_RANGE` | `RangeError` |
| `SQLITE_MISUSE` | `MisuseError` |
| `SQLITE_INTERRUPT` | `InterruptError` |
| `SQLITE_CORRUPT*` | `CorruptError` |

Other SQLite errors map to the phase-specific parent (`OpenError`,
`PrepareError`, `BindError`, or `StepError`) with the SQLite message included.

## 13. Examples

### 13.1. Basic query

```amber
import sqlite3

sqlite3.open("data/app.db", busy_timeout: 2.seconds) |db|:
  db.execute("create table if not exists users(id integer primary key, name text)")
  db.execute("insert into users(name) values (?)", params: ["Ada"])

  db.query("select id, name from users")
```

### 13.2. Prepared statement

```amber
sqlite3.open("data/app.db") |db|:
  db.prepare("insert into users(name) values (:name)") |stmt|:
    ["Ada", "Iris"].each |name|:
      stmt.execute(params: {name: name})
```

### 13.3. Scalar

```amber
count = db.scalar("select count(*) from users")
```

### 13.4. SQL macro in core and convenience calls

```amber
from sqlite3 import sql

threshold_age = 18

count = db.scalar(sql"""
  select count(*)
  from users
  where age > #{threshold_age}
""")

rows = db.select(:users,
  columns: [sql"""count(*) as count"""],
  where: sql"""age > #{threshold_age}""",
  where_not: {role: "admin"}
)

counts = db.select(:events,
  columns: [:kind, sql"""count(*) as count"""],
  group_by: [:kind],
  having: sql"""count(*) > #{minimum_count}"""
)
```

### 13.5. Insert convenience layer

```amber
db.insert(:users,
  columns: [:name, :email],
  values: [
    ["Ada", "ada@example.test"],
    ("Iris", "iris@example.test"),
    {name: "June", email: "june@example.test"}
  ]
)
```

### 13.6. Insert or replace / upsert alias

```amber
db.upsert(:users,
  columns: [:id, :name, :email],
  values: [
    [1, "Ada", "ada@example.test"]
  ],
  returning: [:id]
)
```

### 13.7. JSONB payload when available

```amber
payload = {id: 1, tags: ["compiler", "sqlite"]}

value = if db.jsonb_available?():
  sqlite3.jsonb(payload)
else:
  sqlite3.json(payload)

db.insert(:events,
  columns: [:kind, :payload],
  values: [["created", value]]
)
```

### 13.8. Transaction

```amber
db.transaction(mode: :immediate) |tx|:
  tx.execute("insert into users(name) values (?)", params: ["Ada"])
  tx.execute("insert into users(name) values (?)", params: ["Iris"])
```

### 13.9. Connection pool across fibers or native threads

```amber
import sqlite3
import task

sqlite3.pool("data/app.db", max_size: 10, max_size_per_thread: 2) |pool|:
  jobs = user_ids.map |id|:
    task.spawn:
      pool.checkout |db|:
        db.get(:users, where: {id: id})

  jobs.map |job|:
    job.wait()
```

`Pool` is shareable and may be captured by spawned work. The checked-out
`Database` stays inside the checkout block and is not shared.

### 13.10. Duplicate column names

```amber
result = db.query_arrays(
  "select users.id, posts.id from users join posts on posts.user_id = users.id"
)

result[:columns] # ["id", "id"]
result[:rows][0] # [1, 10]
```

### 13.11. Error handling

```amber
try:
  db.execute("insert into users(id, name) values (?, ?)", params: [1, "Ada"])
rescue sqlite3.ConstraintError |e|:
  "duplicate user"
rescue sqlite3.BusyError |e|:
  "database is busy"
```

## 14. Non-goals for v1

- No ORM, repository, migration DSL, or schema DSL.
- No broad relational-algebra DSL. The v1 convenience layer is limited to
  common DML and simple select/get/update/delete helpers.
- No unsafe SQL interpolation. Interpolation is available only through the
  exportable `sql"""..."""` macro, where ordinary interpolants become bind
  parameters and dynamic SQL syntax must use typed helpers such as
  `sqlite3.ident(...)` or `sqlite3.fragment(...)`.
- No user-defined SQLite scalar functions, aggregates, collations, authorizers,
  progress handlers, or update hooks. They require callbacks into Amber and need
  a separate full-native callback ABI decision.
- No `load_extension` API.
- No nonblocking SQLite execution API. Pool checkout can wait cooperatively in a
  fiber, but `sqlite3_step` and related calls are ordinary blocking native
  database effects.
- No automatic date/time/JSON conversion.
- No encryption/key management API.

## 15. Implementation notes that shape the public API

These are not public methods, but they are required to preserve the API contract.

- Native thunks should use `sqlite3_prepare_v3`, `sqlite3_step`,
  `sqlite3_reset`, `sqlite3_clear_bindings`, and `sqlite3_finalize`.
- `version()`, `source_id()`, and `libversion_number()` should use
  `sqlite3_libversion`, `sqlite3_sourceid`, and `sqlite3_libversion_number`.
- Direct-execution helpers that are not `exec` must inspect the prepare tail and
  reject trailing non-whitespace SQL.
- Values copied out of a row must be copied before the next `sqlite3_step`.
- `Str` bind values use UTF-8 and `SQLITE_TRANSIENT`.
- `Bytes` bind values use `SQLITE_TRANSIENT`.
- The package should call `sqlite3_extended_result_codes(db, 1)` after open.
- The package should install native-only guards for SQL features that can open
  or write files outside the original database path, including `ATTACH` and
  `VACUUM INTO`.
- Pool internals must track the owning native thread for every physical
  connection and must never move an open `sqlite3*` across native threads.
- Pool wait must integrate with Amber's fiber scheduler when a fiber is active,
  and fall back to native mutex/condition-variable waiting for plain native
  thread callers.
- `Pool#checkout` block form must release or discard the lease in an `ensure`
  path, including non-local exits and exceptions.
- `Database`, `Statement`, and `PoolLease` operations must assert their owning
  execution context before calling into SQLite.
- `busy_timeout!` uses SQLite's busy timeout handler. It does not install a
  custom Amber callback.
- `interrupt!` calls `sqlite3_interrupt`.
- JSON feature detection should use both version/build metadata and live
  function probes against the open connection, because compile options can omit
  JSON support.
- Native registration must define every error class listed in section 12.
- The native build tests must assert full native coverage in the JSON summary,
  not merely successful execution.

## 16. References

- SQLite JSON functions and operators: https://www.sqlite.org/json1.html
- SQLite release history: https://www.sqlite.org/changes.html
- Amber macro system design: `DESIGN-macro-system-2026-06-29.md`
- Amber multiline string literals design:
  `DESIGN-multiline-string-literals-2026-06-29.md`
