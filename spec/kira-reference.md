# Kira Language Reference

---

## How This Document Is Organized

Kira is designed so that a beginner can be productive using a small, stable mental model, and reach for more power only when they need it. This reference is organized in the same way.

**Layer 1 — Core** covers everything you need to write real programs: values, functions, types, pattern matching, collections, and error handling. Most code lives here.

**Layer 2 — Intermediate** introduces ownership and borrowing, traits and generics, and async concurrency with `crew`. You will encounter these when writing library code or working with concurrent programs.

**Layer 3 — Advanced** covers the compile-time execution system, dependent types, contracts, concepts, and the low-level `machine` layer. These features exist for when you need to express invariants, generate code, or drop to the metal.

Each section is marked with its layer. You do not need to understand a later layer to use an earlier one. The compiler will point you toward the right layer when you need it.

---

## A Note on Compiler Errors

Kira's compiler is designed to be a teacher, not just a validator. When something goes wrong, error messages follow a consistent format:

```
error[E0012]: type mismatch
  --> src/main.kira:14:5
   |
13 |     let x: int32 = compute()
14 |     process(x)
   |             ^ expected float64, found int32
   |
   = compute() returns int32
   = process() expects float64
   = hint: use float64(x), or change compute()'s return type
```

Error messages always say what was expected, what was found, why the constraint exists, and what you can do about it. When a feature from a later layer would help — for example, when an ownership rule fires — the compiler names the concept and suggests where to read more.

---

# Layer 1 — Core

---

## Syntax Basics

Kira uses Python-style indentation. There are no semicolons, no curly braces for blocks, and no header files. A block is introduced by a colon and an indent; it ends when the indentation returns.

```kira
def greet(name: str) -> str:
    let message = "Hello, {name}!"
    return message
```

All keywords are lowercase. Comments begin with `#`.

```kira
# This is a comment
let x = 42    # so is this
```

A module is one or more files. Every file begins with a `module` declaration naming the module it belongs to; several files may share a name and together form one module.

```kira
module my_app.utils
```

In a module path, the leading segments are folders and the final segment is the module's name. `my_app.utils` is the module `utils` in the folder `my_app/`; the file's own name does not matter.

---

## Built-in Types

All built-in types are lowercase. The types you will use most are:

```kira
bool          # true or false
int32         # 32-bit signed integer (the default for integer literals)
float64       # 64-bit floating point (the default for decimal literals)
str           # UTF-8 text
char          # a single Unicode character
unit          # the "no value" type, like void in other languages
```

The full numeric family, for when you need a specific width:

```kira
# Signed integers
int8  int16  int32  int64  int128

# Unsigned integers
uint8  uint16  uint32  uint64  uint128

# Floating point
float32  float64  float128

# Raw byte
byte    # alias for uint8
```

> **Advanced types** — `isize`, `usize`, and `never` exist but are not needed until later layers. The compiler will introduce them when relevant.

Numeric literals adapt to context. The literal `42` becomes an `int32`, `int64`, or whatever the surrounding code requires. If no context exists, `int32` is the default for integers and `float64` for decimals. A literal that does not fit its context is a compile error.

```kira
let a = 42          # int32 by default
let b: int64 = 42   # int64 because you said so
let c: uint8 = 300  # compile error: 300 does not fit in uint8
```

---

## Integer Overflow

Arithmetic is checked. When the result of `+`, `-`, or `*` does not fit in its type, that is a bug, not a silently wrong answer — it panics, the same way an out-of-bounds index does. Overflow is never undefined behavior.

```kira
let x: int32 = int32.max
let y = x + 1        # panics: int32 overflow
```

The compiler proves overflow away wherever it can — the literal check above (`uint8 = 300`) is this same rule applied at compile time — and inserts a runtime check only where it cannot. ("The result fits" is an ordinary precondition; see Contracts. Release builds may elide these checks with the same flag that elides other contract checks.)

When you want a behavior other than panicking, ask for it explicitly. These operators are ordinary and safe, available everywhere — not only in `machine` code:

```kira
let mixed = h *% 31 +% c     # wrapping (modular) — hashes, checksums, ring buffers
let level = volume +| gain   # saturating — clamps at the type's min or max
```

To handle a possible overflow as a value instead of crashing, use the checked methods, which return `option`:

```kira
match a.checked_add(b):
    some(n) => n
    none    => use_fallback()
```

Division and shifts follow the same principle: dividing by zero, `int32.min / -1`, negating `int32.min`, and shifting by more than the type's width all panic as checked violations, with explicit operators or methods available when another behavior is intended.

---

## Converting Between Types

Kira has no cast operator. To convert a value to another type, call the target type like a constructor:

```kira
let n: int32   = 300
let x: float64 = float64(n)   # int32 -> float64
let b: uint8   = uint8(n)     # narrowing — panics: 300 does not fit in uint8
```

A conversion exists when the target type provides a `from` for the source; the numeric types provide these for one another. Narrowing is checked exactly as arithmetic is: a value that does not fit its destination panics rather than silently truncating. The same `from` mechanism is what `?` uses to convert errors (see Error Handling).

---

## Bindings

`let` creates an immutable binding. `var` creates a mutable one.

```kira
let name = "Alice"      # immutable — name cannot be reassigned
var count = 0           # mutable — count can change
count = count + 1
```

Both infer the type from the value. You can annotate explicitly:

```kira
let score: float64 = 0.0
var remaining: int32 = 10
```

`let` bindings can be shadowed — a new `let` with the same name replaces the old one in the current scope:

```kira
let x = 1
let x = x + 1    # x is now 2; original x is gone
```

---

## Functions

```kira
def add(a: int32, b: int32) -> int32:
    return a + b
```

Type annotations on parameters and return type are required for top-level functions when the types cannot be inferred from context. In practice, the compiler will tell you when an annotation is needed.

Parameter types can be omitted when the compiler can infer them:

```kira
def double(x):
    return x * 2
```

Named arguments and defaults:

```kira
def greet(name: str, loud: bool = false) -> str:
    if loud:
        return "{name}!".to_uppercase()
    return "Hello, {name}"

greet("Alice")                  # uses default: loud = false
greet("Bob", loud: true)        # named argument
```

Functions are values and can be passed around:

```kira
def apply(f: fn(int32) -> int32, x: int32) -> int32:
    return f(x)

let result = apply(double, 5)   # result = 10
```

---

## Lambdas

A lambda is an anonymous function written inline.

```kira
let double = x => x * 2
let add    = (x, y) => x + y

# With type annotations
let add = (x: int32, y: int32) -> int32 => x + y

# Multi-line lambda
let process = x =>:
    let y = transform(x)
    y * 2
```

Lambdas are most useful as arguments to functions like `map` and `filter`:

```kira
let numbers = [1, 2, 3, 4, 5]
let doubled = numbers.map(x => x * 2)
let evens   = numbers.filter(x => x % 2 == 0)
```

---

## Collections

The most common collection for day-to-day use is `list[T]`, a resizable sequence. It is available without any import.

```kira
let names: list[str] = ["Alice", "Bob", "Carol"]
let first = names[0]           # "Alice"
let count = names.len()        # 3

var items: list[int32] = []
items.push(1)
items.push(2)
```

`list` supports the standard operations:

```kira
names.map(n => n.to_uppercase())       # produces a new list
names.filter(n => n.len() > 3)        # keeps elements matching predicate
names.find(n => n.starts_with("A"))   # returns option[str]
names.any(n => n == "Bob")            # returns bool
names.all(n => n.len() > 0)           # returns bool
```

The built-in `array[T, n]` type holds exactly `n` elements, with the length known at compile time. Use it when the size is fixed:

```kira
let zeros: array[float64, 4] = [0.0; 4]    # four zeros
let rgb:   array[uint8,   3] = [255, 0, 0] # red
```

The difference: `list` is a heap-allocated, growable sequence. `array` is a fixed-size sequence whose length is part of its type and known at compile time. For most code, `list` is what you want.

> Other collections — `map[K, V]`, `set[T]`, and others — are in the standard library and covered in the standard library documentation.

---

## Strings

Strings are UTF-8. String literals support interpolation with `{...}`:

```kira
let name = "World"
let msg  = "Hello, {name}!"          # "Hello, World!"
let calc = "2 + 2 = {2 + 2}"         # "2 + 2 = 4"
```

Common string operations:

```kira
let s = "hello"
s.len()                   # 5 (bytes) — use s.chars().count() for characters
s.to_uppercase()          # "HELLO"
s.contains("ell")         # true
s.starts_with("he")       # true
s.replace("l", "r")       # "herro"
s.split(",")              # list[str]
"{s} world"               # "hello world"
```

---

## Control Flow

### `if`

```kira
if score > 90:
    return "A"
elif score > 80:
    return "B"
else:
    return "C"
```

`if` is also an expression:

```kira
let label = "pass" if score > 90 else "fail"
```

### `while`

```kira
var n = 1
while n < 100:
    n = n * 2
```

### `for`

```kira
for i in 0..10:          # 0, 1, ..., 9
    println(i)

for i in 0..=10:         # 0, 1, ..., 10
    println(i)

for name in names:
    println("Hello, {name}")
```

`for` is also an expression that collects results:

```kira
let squares = for x in 1..5 => x * x     # [1, 4, 9, 16, 25]
let evens   = for x in 0..20 if x % 2 == 0 => x
let pairs   = for x in 0..3, y in 0..3 => (x, y)
```

---

## Pattern Matching

`match` is Kira's primary way of making decisions. It is an expression, meaning it produces a value. The compiler checks that all possible cases are handled — if you forget one, it is a compile error.

```kira
let description = match score:
    100      => "perfect"
    90..=99  => "excellent"
    70..=89  => "good"
    _        => "needs work"
```

`match` shines with sum types (see Types section):

```kira
type shape =
    | circle(float64)
    | rect(float64, float64)

def area(s: shape) -> float64:
    match s:
        circle(r)    => 3.14159 * r * r
        rect(w, h)   => w * h
```

Guard clauses narrow a pattern further:

```kira
match s:
    circle(r) if r <= 0.0 => 0.0
    circle(r)             => 3.14159 * r * r
    rect(w, h)            => w * h
```

Patterns can destructure tuples and structs:

```kira
let point = (3.0, 4.0)
let (x, y) = point

type person = { name: str, age: int32 }
let { name, age } = someone
```

---

## Type Declarations

### Structs

A struct groups named fields:

```kira
type point = { x: float64, y: float64 }

let p = point { x: 1.0, y: 2.0 }
let d = p.x * p.x + p.y * p.y
```

### Sum Types

A sum type (also called an enum or tagged union) represents a value that is exactly one of several variants:

```kira
type direction = north | south | east | west

type shape =
    | circle(float64)            # holds a radius
    | rect(float64, float64)     # holds width and height
    | point                      # holds nothing
```

Constructors are lowercase. You always use `match` to inspect a sum type.

### Type Aliases

```kira
type meters   = float64
type name_map = list[(str, str)]
```

---

## Error Handling

Kira has no exceptions. Functions that can fail return `result[T, E]`, where `T` is the success value and `E` is the error. Functions that might produce nothing return `option[T]`.

```kira
type option[T] = some(T) | none
type result[T, E] = ok(T) | err(E)
```

These are ordinary sum types. You inspect them with `match`:

```kira
def find_user(id: int32) -> option[user]:
    ...

match find_user(42):
    some(u) => println("Found: {u.name}")
    none    => println("Not found")
```

The `?` operator makes error propagation concise. Inside a function that returns `result`, `?` unwraps `ok` or returns `err` early:

```kira
def load_config(path: str) -> result[config, io_error]:
    let text   = read_file(path)?     # read_file's error is io_error
    let parsed = parse_toml(text)?    # parse_toml's error is parse_error
    return ok(parsed)
```

The two calls fail with *different* error types — `io_error` and `parse_error` — yet both propagate out of a function whose error type is `io_error`. When the propagated error is not already the function's error type, `?` converts it, using the same `from` mechanism as an ordinary type conversion (see Converting Between Types). This is the one place Kira converts implicitly, and it is unambiguous because the target is fixed by the function's return type. If no conversion from the source error to the target exists, the compiler says so and names the impl to add:

```
error[E0021]: cannot propagate `parse_error` with `?`
   = load_config() returns errors of type io_error
   = no conversion from parse_error to io_error exists
   = hint: add `impl from[parse_error] for io_error`, or return a wider error type
```

`?` behaves the same way in a function that returns `option`: `e?` yields the inner value on `some`, and returns `none` from the function on `none`.

Without `?`, every step would need a `match`. With it, the happy path is linear and errors propagate automatically.

Errors are just values — define them as types:

```kira
type app_error =
    | file_not_found(str)
    | permission_denied(str)
    | parse_failed(str)
```

**Panics** are different from errors. A panic means a bug in your program — an index out of bounds, an `unwrap()` on `none` when you were certain it held a value. Panics are not for expected failure conditions. Use `result` for those.

```kira
let v = some_list[999]      # panics if index is out of bounds
let x = opt.unwrap()        # panics if opt is none
panic("should never reach here")
```

---

## Modules and Imports

Each file's first line names the module it belongs to. A module may span several files:

```kira
module my_app.geometry
```

To use something from another module, `use` it:

```kira
use my_app.geometry.point
use my_app.geometry.{ point, shape }   # multiple at once
use my_app.geometry.point as pt        # rename
```

Visibility controls what other code can see. The default is `module` — visible across every file of the current module, but not to outside code. Mark things `pub` to export them; mark them `file` to keep them private to the file they are written in:

```kira
module my_app.geometry

pub type point = { pub x: float64, pub y: float64 }

pub def distance(a: point, b: point) -> float64:
    let dx = a.x - b.x
    let dy = a.y - b.y
    sqrt(dx*dx + dy*dy)

file def scratch_helper() -> float64:    # file-private — not visible in other files
    ...
```

Visibility levels in full:

```
pub     visible to any importer
module  visible across every file of this module (default)
file    visible within this file only
```

### Re-exporting

`pub use` brings a name in from another module and re-exports it as part of this module's own public surface. This is how a module presents a curated public API — a facade that gathers names from several places into one import point:

```kira
module my_app

pub use my_app.geometry.{ point, shape }
pub use my_app.transform.rotate
```

Importers of `my_app` now see `point`, `shape`, and `rotate` directly.

---

## Programs and `main`

An executable program has one entry point: a function named `main`. The simplest form takes no arguments and returns `unit`:

```kira
def main() -> unit:
    println("Hello, world!")
```

To use `?` inside `main`, give it a `result` return type. A returned `err` ends the program with a non-zero exit status and reports the error:

```kira
def main() -> result[unit, app_error]:
    let cfg = load_config("app.toml")?
    run(cfg)
    return ok(unit)
```

Command-line arguments and environment variables come from prelude functions rather than parameters, so `main`'s signature stays uniform:

```kira
let arguments = args()        # list[str]
let home      = env("HOME")   # option[str]
```

The compiler uses the `main` in the project's entry module (set in `project.kira`) as the program's start. A library has no `main`.

---

# Layer 2 — Intermediate

---

## Ownership and Borrowing

Kira tracks which part of your code owns each value. When a value's owner goes out of scope, the value is freed. This is how Kira manages memory without a garbage collector and without `malloc`/`free`.

Most of the time you do not think about ownership. The compiler handles it silently. You only need to think about it when you want to share or pass data without copying it — which is where *borrowing* comes in.

### Ownership

When you assign a value or pass it to a function, ownership transfers by default:

```kira
def process(data: list[int32]) -> int32:    # takes ownership of data
    ...

let numbers = [1, 2, 3]
let result = process(numbers)
# numbers is no longer accessible here — process owns it now
```

If you need to use `numbers` after the call, you have two options: copy it, or lend it.

### Borrowing

A *borrow* lends a value to a function for the duration of a call. The owner keeps ownership; the callee may use the value but not move it. Crucially, a borrow is a way of *passing* a value, not a value in its own right: you cannot store a borrow in a variable that outlives the call, return one, or place one in a struct field or collection. Because every borrow is bounded by the call that created it, Kira has no lifetime annotations — there is nothing to name.

Mark the parameter with `&`, and lend the value with `&` at the call site:

```kira
def sum(data: &list[int32]) -> int32:    # borrows data, does not take ownership
    var total = 0
    for x in data: total += x
    return total

let numbers = [1, 2, 3, 4, 5]
let total = sum(&numbers)                # lend it; numbers is still accessible
println("sum: {total}, list: {numbers}")
```

`&mut` lends mutable access. It is required at the call site too, so that mutation through a call is always visible where it happens:

```kira
def double_all(data: &mut list[int32]) -> unit:
    for i in 0..data.len():
        data[i] = data[i] * 2

var numbers = [1, 2, 3]
double_all(&mut numbers)    # the &mut marks the mutation; numbers is now [2, 4, 6]
```

The rules the compiler enforces:
- Any number of immutable borrows (`&`) may exist simultaneously.
- At most one mutable borrow (`&mut`) may exist at a time.
- A mutable borrow cannot coexist with any immutable borrows.
- A borrow cannot escape the call it was made for — it cannot be returned, stored in a field, or placed in a collection.

When you violate these rules, the compiler explains what went wrong and why:

```
error[E0013]: cannot borrow `numbers` as mutable — already borrowed as immutable
  --> src/main.kira:8:5
   |
6  |     let view = &numbers          # immutable borrow starts here
7  |     double_all(&mut numbers)     # mutable borrow attempted here
   |                ^^^^^^^^^^^^ mutable borrow
   |
   = `view` holds an immutable borrow until line 10
   = hint: move the mutable borrow after `view` is last used
```

### Views: Slices and Strings

Since a borrow cannot escape a call, how do you hand back a *window* into a collection — the first half of a list, a substring? For that Kira has **view types**, the one kind of borrowing value the language permits:

```kira
slice[T]       # a read-only view of a contiguous run of elements
slice_mut[T]   # a mutable view
str            # a read-only view of UTF-8 text (already familiar)
```

A view is produced by slicing, and unlike a bare borrow it may be passed *and* returned:

```kira
def first_half[T](xs: &list[T]) -> slice[T]:
    xs[0 .. xs.len() / 2]

let data  = [1, 2, 3, 4, 5, 6]
let front = first_half(&data)    # a view; data stays borrowed while front is alive
```

A view's borrow is still tracked — a view can never outlive the collection it looks into — but you never write a lifetime for it. The compiler infers that a returned view borrows from the argument it was sliced from, and keeps the source borrowed for as long as the view lives. When you need a result that escapes and stands on its own, return an owned value or a handle (an index) rather than a view.

### Closures and Capture

A lambda captures the variables it uses from the surrounding scope. *How* it captures depends on whether the closure escapes:

- A closure that does **not** escape — passed to `map`, `filter`, or any function that calls it and returns — captures by **borrow**. Nothing is copied, and the borrow is bounded by the call, just like any other borrow.
- A closure that **does** escape — stored somewhere longer-lived or sent to another task — captures by **move**, taking ownership of what it uses, because a borrow cannot escape. Write `move` before the lambda to force move-capture explicitly.

```kira
let factor = 3
let scaled = numbers.map(x => x * factor)   # borrows factor; does not escape
```

Structured concurrency is where this pays off. Because a `crew`'s tasks cannot outlive the `crew`, and the `crew` cannot outlive its enclosing scope, a closure spawned into a `crew` may **borrow** local data — there is no `'static`-style requirement and no forced `move`:

```kira
async def totals() -> result[summary, error]:
    let rows = load_rows()
    crew c:
        let a = c.spawn(sum_column(&rows, 0))   # borrows rows across the crew
        let b = c.spawn(sum_column(&rows, 1))
    let total = a.get()? + b.get()?
    return ok(total)
```

### Shared Ownership

When single ownership is genuinely too restrictive — for example, a value that needs to be held by multiple parts of a program simultaneously — use `shared`:

```kira
let config: shared config_t = shared load_config("app.toml")

# config can now be cloned freely; both copies refer to the same data
let worker_config = config.clone()
```

`shared` values are *atomically* reference-counted, so a single `shared` type is always safe to hand to another task. A `shared` handle gives **read-only** access to what it points at — it never yields `&mut` — so any number of tasks may hold and read the same `shared` value at once without a data race. To *mutate* data behind a `shared`, wrap it in a synchronized cell such as `mutex[T]` (see Data-Race Freedom). `shared` carries a small runtime cost; prefer single ownership, and prefer scoped borrowing — which is free — whenever you can.

---

## Traits

A trait defines a set of capabilities a type can have. Traits are how Kira expresses "this function works for any type that supports X."

### Defining and Implementing a Trait

```kira
trait show:
    def show(self) -> str

type point = { x: float64, y: float64 }

impl show for point:
    def show(self) -> str:
        "({self.x}, {self.y})"
```

Now any function that accepts a `T: show` can call `.show()` on a value of type `T`.

Traits can provide default implementations:

```kira
trait greet:
    def name(self) -> str

    def greeting(self) -> str:           # default — may be overridden
        "Hello, {self.name()}!"
```

### Generic Functions

A generic function works for any type satisfying a bound:

```kira
def print_item[T: show](item: T) -> unit:
    println(item.show())

def largest[T: ord](items: &list[T]) -> option[usize]:
    if items.is_empty(): return none
    var best = 0
    for i in 1..items.len():
        if items[i] > items[best]: best = i
    return some(best)    # return the index — a borrow could not escape this call
```

`[T: show]` means "T is any type that implements show." Multiple bounds use `+`:

```kira
def inspect[T: show + eq](a: T, b: T) -> unit:
    if a == b:
        println("equal: {a.show()}")
    else:
        println("{a.show()} != {b.show()}")
```

### `requires` — Trait Dependencies

A trait can require that implementing types also implement other traits. This is expressed with `requires`:

```kira
trait ord requires eq:
    def cmp(self, other: &self) -> ordering
```

Any type implementing `ord` must first implement `eq`. When you have a bound `T: ord`, you can also call `eq` methods on `T` without adding `T: eq` — the compiler knows `ord` implies it.

### Deriving Common Traits

Many common trait implementations can be generated automatically:

```kira
type color = red | green | blue
    deriving eq, ord, show, hash

type point = { x: float64, y: float64 }
    deriving eq, show
```

`deriving` generates the implementation the same way you would write it by hand. You can override specific methods while keeping the rest derived.

### Operator Overloading

Operators are traits. To make a type work with `+`, implement the `add` trait:

```kira
trait add:
    type output
    def add(self, other: self) -> self.output

type vec2 = { x: float64, y: float64 }
    deriving show, eq

impl add for vec2:
    type output = vec2
    def add(self, other: vec2) -> vec2:
        { x: self.x + other.x, y: self.y + other.y }

let a = vec2 { x: 1.0, y: 2.0 }
let b = vec2 { x: 3.0, y: 4.0 }
let c = a + b    # vec2 { x: 4.0, y: 6.0 }
```

---

## Async and Concurrency

### The Basics: `async` and `await`

Mark a function `async` to make it a suspendable computation. Inside an async function, `await` pauses execution until another async operation completes, freeing the current thread for other work.

```kira
async def fetch_name(id: int32) -> result[str, network_error]:
    let response = await http.get("https://api.example.com/users/{id}")?
    let body     = await response.text()?
    return ok(body)
```

`async` functions return `task[T, E, C]` — a value describing the computation. Nothing runs until something awaits or runs the task.

### `crew` — Structured Concurrency

`crew` is the way to run multiple async tasks together. A `crew` block waits for all its tasks before continuing. If any task fails, the rest are cancelled and the error propagates.

```kira
async def fetch_dashboard(user_id: int32) -> result[dashboard, error]:
    crew c:
        let user_task   = c.spawn(fetch_user(user_id))
        let orders_task = c.spawn(fetch_orders(user_id))
        let prefs_task  = c.spawn(fetch_preferences(user_id))

    let user   = user_task.get()?
    let orders = orders_task.get()?
    let prefs  = prefs_task.get()?
    return ok(build_dashboard(user, orders, prefs))
```

Tasks in a `crew` cannot outlive the `crew` block. This is enforced by the compiler — there are no dangling background tasks.

### Parallel Composition with `par`

`par` runs a fixed set of tasks simultaneously and gives you all the results as a tuple:

```kira
let (user, orders, prefs) = await par:
    fetch_user(id)
    fetch_orders(id)
    fetch_preferences(id)
```

If any task fails, the others are cancelled.

### Racing with `race`

`race` runs multiple tasks and returns the first to complete, cancelling the rest:

```kira
let result = await race:
    fetch_from_primary(key)
    fetch_from_replica(key)
```

### Moving Work to a Different Context

`on` runs a block of work on a different executor and returns the result to the current context:

```kira
async def handle_request(req: http_request) -> http_response:
    # we're on the io context
    let result = await on(pool):
        expensive_computation(req.body)    # runs on the thread pool
    # back on io context here
    return http_response.ok(result)
```

### Error Handling in Concurrent Code

Errors from concurrent tasks follow the same `result` model. A `crew` that uses `on_error: collect` gathers all errors rather than stopping on the first:

```kira
crew c(on_error: collect):
    for item in items:
        c.spawn(process(item))

for err in c.errors():
    log("failed: {err.message()}")
```

### Data-Race Freedom

Kira's memory-safety rule — many readers or one writer, never both — is also its data-race rule. The compiler extends that one invariant across tasks, so concurrent code is checked by the same mechanism as sequential code.

Two concepts, both satisfied automatically (you never write `impl` for them, and they stay invisible until one is violated), decide what may cross a task boundary:

- **`send`** — a type whose ownership may move to another task.
- **`share`** — a type that several tasks may read through `&` at the same time.

A type satisfies them from its parts: a struct of `send` fields is `send`, and so on. The concurrency primitives require them where it matters — `c.spawn`, `par`, `on`, and `channel.send` move `send` values and borrow `share` values — and when a type qualifies for neither, the compiler names the concept and explains why (a raw pointer, for instance, is neither).

There are two ways to share data between tasks, and they are the same invariant enforced at two different times.

**Scoped sharing — free, checked at compile time.** Because a `crew`'s tasks cannot outlive it, spawned tasks may borrow data from the enclosing scope. The ordinary borrow rules apply across the crew: any number of tasks may hold `&data`, but the compiler will not let two tasks hold `&mut data`. Concurrent mutable aliasing is a compile error, and no lock is involved.

```kira
let table = build_table()
crew c:
    c.spawn(lookup(&table, "a"))   # many readers — fine
    c.spawn(lookup(&table, "b"))
```

**Unscoped sharing — `shared`, with synchronized mutation.** When data must outlive any single scope, a `shared[T]` handle keeps it alive and gives read-only access, so many tasks can read at once. To *mutate* shared data, wrap it in a synchronized cell that grants exclusive access one holder at a time:

- `mutex[T]` — lock to obtain a temporary `&mut T`; other tasks wait.
- `rwlock[T]` — many readers or one writer, decided at runtime.
- `atomic[T]` — lock-free operations on primitive values.

```kira
let counter: shared mutex[int32] = shared mutex(0)

crew c:
    for _ in 0..n:
        c.spawn(async:
            let guard = counter.lock()   # exclusive access, released when guard drops
            *guard += 1
        )
```

A `mutex` enforces "one writer at a time" at runtime, exactly as the borrow checker enforces it at compile time — which is why the two guarantees are one guarantee. Reach for `shared mutex[T]` only when data genuinely escapes structure; scoped sharing costs nothing and should be the default.

---

## The Module System in Depth

### Modules Span Files

A module is not a single file, and modules do not nest. Any number of files may declare the same module; together they form it, and `module`-visible names are shared across all of them. Two modules whose paths share a prefix — `my_app.geometry` and `my_app.geometry.shapes` — are unrelated: the shared prefix is a shared folder, not a parent–child relationship. Neither can see the other's non-`pub` names.

To split a large module across files, give each file the same `module` line:

```kira
# my_app/geometry/core.kira
module my_app.geometry

pub type point = { pub x: float64, pub y: float64 }
```

```kira
# my_app/geometry/distance.kira
module my_app.geometry

pub def distance(a: point, b: point) -> float64:    # sees point directly
    ...
```

### Project Structure

A project's root is `project.kira`, and it is ordinary Kira evaluated at compile time — there is no separate manifest language. Search paths and dependencies are `static` data:

```kira
module my_app

static search_path: list[str] = ["src", "vendor"]

static deps: list[dependency] = [
    dependency { name: "geometry", path: "vendor/geometry", version: "1.2.0" },
]
```

`dependency` is a type the build system provides. Because the manifest is Kira, dependency lists can be computed — assembled with `for`, branched on with `static if`, or factored into helper functions — in the same language as the rest of the program.

Dependencies are resolved at compile time, and a `project.lock` file records the resolved versions. What a package exposes to its dependents is determined by the `pub` surface of its modules, discovered through compile-time reflection — there is no separate list of exported modules to maintain.

---

# Layer 3 — Advanced

---

## Compile-Time Execution

Any expression that does not depend on runtime input can be evaluated at compile time. There is no separate template language or macro system — the same functions, loops, and pattern matching you use at runtime are available at compile time.

`static` forces compile-time evaluation and is a compile error if the expression cannot be resolved:

```kira
static PI:    float64        = 3.141592653589793
static LIMIT: int32          = 1_000_000
static TABLE: array[int32, 256] = build_lookup_table()

def build_lookup_table() -> array[int32, 256]:
    var t = [0; 256]
    for i in 0..256:
        t[i] = i * i
    return t
```

`build_lookup_table` is an ordinary function. The compiler runs it at compile time because `TABLE` is `static`. The result is baked into the binary.

### `static if`

Selects between code paths at compile time. The non-taken branch is not compiled or type-checked:

```kira
static IS_64BIT: bool = size_of[usize]() == 8

static if IS_64BIT:
    type word = uint64
else:
    type word = uint32
```

### `static assert`

A compile error if the condition is false when statically knowable, a runtime panic otherwise:

```kira
static assert size_of[T]() <= 64, "T must fit in a cache line"
```

### `static for`

Unrolls a loop at compile time, generating one code path per iteration:

```kira
def field_names[T]() -> list[str]:
    static for field in T.fields() => field.name
```

### Compile-Time Reflection

Types expose their structure as compile-time values:

```kira
T.fields()          # list of field descriptors
T.field_count()     # number of fields (compile-time integer)
T.name()            # name of the type as str
```

Combined with `static for`, this lets you generate code from type structure — the mechanism behind `deriving`.

### Pure Functions

`pure` asserts that a function is referentially transparent: same inputs always produce the same output, with no side effects. The compiler verifies this claim.

```kira
pure def clamp(x: float64, lo: float64, hi: float64) -> float64:
    if x < lo: return lo
    if x > hi: return hi
    return x
```

`pure` and `static` are independent. A function can be:
- `pure` only — effect-free at runtime, not necessarily evaluated at compile time
- `static` only — evaluated at compile time, but may have compile-time-side effects (diagnostics)
- `static pure` — both: effect-free and compile-time evaluable

```kira
static pure def align_up(n: usize, align: usize) -> usize:
    (n + align - 1) & ~(align - 1)

# Usable in types and at runtime
type aligned_buf[n: usize] = array[byte, align_up(n, 16)]
```

`pure` functions may appear freely in contract conditions. Impure functions may not.

`pure` lambdas use the `pure` keyword before the arrow:

```kira
let square = pure x => x * x
```

### Quoting and Splicing

Quoting and splicing let you construct and emit code as compile-time values. A backtick captures syntax; `~` inserts it:

```kira
static let increment: expr = `(x + 1)`

def apply(x: int32) -> int32:
    return ~increment    # equivalent to: return x + 1
```

Building expressions programmatically:

```kira
static def make_adder(n: int32) -> expr:
    `(x + ~(expr.lit(n)))`

static let add5 = make_adder(5)

def apply(x: int32) -> int32:
    return ~add5    # equivalent to: return x + 5
```

Generating definitions — the primary use case. This is how `deriving` works internally:

```kira
static def derive_show[T]() -> def_expr:
    let fields = T.fields()
    let parts  = for f in fields =>
        `(~(expr.lit(f.name)) + ": " + self.(~(expr.field(f))).show())`
    let body   = parts.join(` + ", "`)
    `impl show for ~(expr.ty[T]()):
        def show(self) -> str:
            ~body`

~derive_show[point]()    # emits the impl at this point in the module
```

Quotes are hygienic by default — names inside a quote do not capture names at the splice site. Use `expr.capture("name")` to deliberately capture a name from the surrounding context.

Quote value types:

```kira
expr       # a single expression
stmt       # a single statement
def_expr   # a definition (function, type, impl, etc.)
type_expr  # a type expression
```

All are compile-time only.

---

## Compile-Time Semantics

`static` evaluation, reflection, contracts, and dependent types all run before the program does. Together they make compile time a second execution environment, and the sections above describe each feature's syntax in isolation. This section describes the environment they share: what runs there, what it may do, and how the features interact.

### Two Axes: Phase and Effect

Two independent properties classify every function.

*Phase* — when it runs. Runtime code runs in the finished program. Compile-time code runs in the compiler; `static` forces it. Ordinary code is runtime code that the compiler *may* also evaluate at compile time when its inputs are known.

*Effect* — whether it is `pure` (referentially transparent) or may cause effects. This is independent of phase: a function can be pure at runtime, effectful at runtime, pure at compile time, or effectful at compile time.

The two axes do not collapse into one another, and most confusion about `pure` versus `static` comes from treating them as the same axis. They are not: `pure` is about *effects*, `static` is about *phase*.

### Two Subsystems: Evaluation and Reasoning

Compile time has two distinct machineries, and knowing which one is acting resolves most interaction questions.

*Evaluation* is an interpreter that runs ordinary Kira — loops, functions, pattern matching — to produce compile-time values. `build_lookup_table()` and `deriving` are evaluation.

*Reasoning* is the constraint solver that discharges refinement predicates, contract conditions, and dependent-type obligations. It *proves*; it does not run arbitrary code.

Contracts sit on the seam: a contract *condition* is produced by evaluation, but whether it *holds* is settled by reasoning when possible, and by a runtime check otherwise.

### Compile-Time Memory

Compile-time evaluation may allocate freely, on a heap that exists only during compilation. Nothing on that heap reaches the binary unless it is *reified* into a `static` constant — like `static TABLE` — at which point it becomes frozen, immutable data baked into the program.

There is no persistent, program-global, mutable compile-time state. Compile-time evaluation is **confluent**: its results never depend on the order in which the compiler elaborates the program. This is what keeps compilation deterministic, parallel, and incremental — a module's meaning cannot depend on side effects left behind by another.

### Effects at Compile Time

Compile-time code may cause exactly two effects: **emitting diagnostics** (as `static assert` does) and **emitting code** (as quoting and splicing do). Neither is observable *as data* by other compile-time code — you cannot read back which diagnostics or definitions have been emitted. So even effectful `static` code stays confluent, and the order in which the compiler happens to run it cannot change the program.

### Reflection

Reflection reads the program's structure — types, fields, names, the members of a module. That structure is immutable, so reflection is referentially transparent, and a `pure` function may use it freely:

```kira
pure def field_count[T]() -> usize:
    T.field_count()
```

Reflection resolves at compile time. Because Kira monomorphizes and has no dynamic dispatch, every type is known statically, so the reflection above is computed once per instantiation and its result — an ordinary constant — flows into runtime like any other value. There is no *runtime* reflection: a value carries no type tag to interrogate.

Reflection may invoke only pure functions. It therefore can never cause an effect, which is exactly what makes it safe to use from inside a contract.

### Declarative Queries, Not Registries

Because there is no mutable global compile-time state, you do not accumulate program-wide information by having each definition register itself. You state the shape you want and let the compiler answer it against the finished program:

```kira
# every type that satisfies the `command` concept, gathered at compile time
static COMMANDS: map[str, command] = map(
    for t in types_implementing[command]() => (t.name(), make_command(t))
)
```

A query observes the complete, elaborated program and returns the same result no matter the elaboration order — "this is the state I want," not "make the state this." A whole-program query naturally depends on the whole program, so it is resolved in a late, post-elaboration phase; its result is still a pure function of the final program.

### How the Pieces Interact

| Question | Answer |
|---|---|
| Can a contract use reflection? | Yes. Reflected data is immutable and resolves to a compile-time constant, usable by the static verifier and, when reified, by a runtime check. |
| Can reflection call pure functions? | Yes — and only pure functions, so no effect can leak through it. |
| Can a pure function inspect types? | Yes. Type reflection is referentially transparent; it resolves at compile time and its result flows into runtime as a constant. |
| Can reflection allocate? | Yes, on the compile-time heap. Nothing survives compilation except values reified into `static` constants. |
| Can compile-time code mutate global state? | No. Compile-time evaluation is confluent; program-wide information comes from declarative queries, never mutation. |

---

## Dependent Types

A dependent type is a type that contains a value. This lets the compiler reason about properties like buffer lengths, array indices, and state machine states at compile time.

```kira
# vec[T, n] carries its length in the type
type vec[T, n: usize] = { data: array[T, n] }

def concat[T, m: usize, n: usize](a: vec[T, m], b: vec[T, n]) -> vec[T, m + n]:
    ...

# head requires length >= 1 — calling head on an empty vec is a compile error
def head[T, n: usize](v: vec[T, n + 1]) -> T:
    ...
```

### Refinement Types

A refinement constrains a type with a predicate. The compiler uses a constraint solver to verify predicates statically where possible:

```kira
type positive       = int32 where self > 0
type index[n: usize] = usize where self < n

def safe_get[T, n: usize](v: array[T, n], i: index[n]) -> T:
    ...
```

When the compiler cannot prove the predicate statically, you prove it with a runtime check that returns a typed proof:

```kira
if let some(i) = index[n].try_from(raw_index):
    v[i]    # i is now index[n] — access is safe, no bounds check needed
```

### State Machines in Types

Types parameterized over state values enforce valid operation sequences at compile time:

```kira
type connection[S: conn_state]

def connect(addr: str)              -> connection[closed]: ...
def open(c: connection[closed])     -> connection[open]:   ...
def send(c: connection[open], data: bytes) -> unit:        ...
def close(c: connection[open])      -> connection[closed]: ...
```

Calling `send` on a closed connection is a compile error. No runtime state field needed — the state is erased after compilation.

---

## Contracts

Contracts attach preconditions, postconditions, and invariants to functions and types. The compiler verifies them statically when it can and enforces them at runtime otherwise.

```kira
def sqrt(x: float64) -> float64
    pre  x >= 0.0
    post return >= 0.0:
    ...
```

In a postcondition, `return` names the value the function returns. (It is deliberately not called `result`, which would collide with the `result` type.)

Multiple conditions, with optional messages:

```kira
def reserve(buf: &mut buffer, n: usize) -> unit
    pre  n > 0,         "cannot reserve zero bytes"
    pre  n < max_alloc, "requested size exceeds maximum"
    post buf.capacity >= n:
    ...
```

Struct invariants are checked at construction and mutation boundaries:

```kira
type positive_int = { value: int32 }
    invariant self.value > 0
```

When a condition is statically knowable, violation is a compile error. When it depends on runtime values, violation panics in debug builds. Release builds may elide runtime contract checks with an explicit flag — doing so is the programmer's assertion that all contracts hold by other means.

Only `pure` functions may appear in `pre`/`post` conditions. This guarantees that contract evaluation has no side effects.

---

## Concepts

A concept is a named bundle of constraints that a type must satisfy. Unlike traits, you never write `impl concept for type` — a type either satisfies a concept or it does not, based on what it already provides.

```kira
concept sortable[T]:
    T: ord + show
    size_of[T]() <= 64    # compile-time value constraint

concept network_value[T]:
    T: show + eq + hash + into[bytes] + from[bytes]
```

Use a concept as a bound:

```kira
def sort_and_display[T: sortable](items: &mut list[T]) -> unit:
    items.sort()
    for x in items: println(x.show())
```

Concepts compose with `+`:

```kira
def process[T: sortable + network_value](val: T): ...
```

**Traits vs concepts:** a trait defines behavior that a type opts into by writing `impl`. A concept defines a constraint that a type satisfies automatically if it already provides the required capabilities. Traits are for defining what a type *can do*; concepts are for constraining what a generic function *requires*.

---

## Modules as Compile-Time Values

At compile time, a module is a value: a record of the types, functions, and constants it defines. You rarely bind one to a variable, but because a module is a value, it can be *parameterized over*, *passed to* other modules, and *reflected on* — all at compile time. Modules are never runtime values, so none of this adds dynamic dispatch or any run-time cost.

### Signatures

A `signature` describes the shape a module must have — the module-level analogue of a concept. It lists required types, functions, and constants without implementing them:

```kira
signature backend:
    type conn
    def connect(url: str) -> conn
    def query(c: &conn, sql: str) -> result[rows, db_error]
    def close(c: conn) -> unit
```

A module *satisfies* a signature structurally, the same way a type satisfies a concept: if it provides the matching members, it qualifies — there is no `impl` to write. A `postgres` module and a `sqlite` module that both provide `conn`, `connect`, `query`, and `close` each satisfy `backend`.

### Parameterized Modules

A module may take compile-time parameters — types, values, and other modules — using the same `[...]` syntax that types and functions use. A module parameter is bounded by a signature:

```kira
module audited[DB: backend]:
    pub def query(c: &DB.conn, sql: str) -> result[rows, db_error]:
        audit_log(sql)
        DB.query(c, sql)
```

`audited` is a function from a module to a module — a functor — written in ordinary module syntax. Instantiating it with a concrete module produces a concrete module:

```kira
use audited[postgres] as db

let rows = db.query(&conn, "select 1")   # audited, backed by postgres
```

Parameterization composes with the rest of the compile-time facilities: a parameterized module can branch with `static if`, unroll with `static for`, and synthesize members with quoting and splicing when it must build them programmatically.

### Reflecting on a Module

A module exposes its members as compile-time values, mirroring the reflection available on types:

```kira
M.functions()   # list of function descriptors
M.types()       # list of type descriptors
M.name()        # the module's name, as str
```

Reflection respects visibility: from outside a module only its `pub` members are visible; from within, all of them are.

### Generating Code From a Module

Reflection plus `static for` turns a module into a source of generated code — which is how Kira does the jobs other languages reach for dynamic dispatch to solve. Here a command table is built from every public function of a module, entirely at compile time:

```kira
static COMMANDS: map[str, command] = map(
    for f in cli_commands.functions() => (f.name(), make_command(f))
)
```

No registry is populated at run time and no virtual call is made; the table is baked into the binary.

### Selecting a Module at Compile Time

Because module selection happens at compile time, `static if` gives you zero-cost dependency injection — swap a real implementation for a fake in tests with no runtime indirection:

```kira
static if BUILD.test:
    use fake_io  as io
else:
    use real_io  as io
```

Both `fake_io` and `real_io` satisfy the same signature, so the code that uses `io` never changes.

---

## The `machine` Layer

`machine` is a function prefix granting access to low-level machine details. Inside a `machine` function, the compiler makes no safety guarantees. Use it only when you need to cross into territory that Kira's ownership and type system cannot otherwise express.

```kira
machine def fast_sum(data: slice[float32], len: usize) -> float32:
    let p = data.as_ptr()
    var sum: float32 = 0.0
    for i in 0..len:
        sum += *(p + i)
    return sum
```

Available inside `machine` functions:
- Raw pointer types `*T` and `*mut T`
- Pointer arithmetic
- Explicit memory layout (`layout`, `align`, `offset`)
- SIMD intrinsics
- Unsafe casts (`transmute`)
- Inline assembly: `asm { ... }`

Prefixes compose:

```kira
async machine def dma_transfer(src: *byte, dst: *mut byte, n: usize) -> result[unit, dma_error]:
    ...

pure machine def read_le_u32(p: *uint32) -> uint32:
    *p
```

Outside `machine` functions the compiler assumes no pointer aliasing and no invalid memory. Contracts are permitted on `machine` functions; all checks within them are runtime-only.

---

## Advanced Concurrency: Execution Graphs

> You probably do not need this yet. `async`/`await` + `crew` covers the vast majority of concurrency needs. Reach for execution graphs when you need to hand a computation pipeline to an external scheduler, add instrumentation across pipeline stages, or run stages on heterogeneous hardware (e.g. GPU, DMA engine, DSP).

An execution graph is a pipeline of work described as a value. Nothing runs until you connect the graph to a scheduler and start it.

```kira
let graph =
    just(input)
    | then(parse)
    | on(pool, then(transform))
    | on(io, then(write_result))
    | with_timeout(30s)
    | retry(max: 3, backoff: exponential)

let handle = graph.connect(my_scheduler).start()
let result = await handle
```

Graphs are inspectable at compile time:

```kira
static STAGES:   usize = count_stages(graph)
static USES_IO:  bool  = graph.uses_context(io)
```

Custom schedulers receive the graph as a value and may reorder stages, add instrumentation, or dispatch to specialized hardware. This is the extension point for game engines, real-time systems, and GPU pipelines.

---

## Advanced Cancellation

Every task carries a cancellation token from its parent `crew`. Cancellation is cooperative — tasks check for it at suspension points or explicitly:

```kira
async def long_work() -> result[unit, cancelled]:
    for i in 0..1_000_000:
        if cancel.is_requested(): return err(cancelled)
        step(i)
        await yield    # explicit yield + cancellation check
    return ok(unit)
```

`await yield` is a suspension point that does no real waiting — it gives the scheduler a chance to run other work and checks for cancellation.

Cancel a `crew` from outside:

```kira
let handle = crew c:
    c.spawn(long_work())

handle.cancel()
await handle    # waits for clean shutdown
```

---

## Channels

Channels pass ownership of values between tasks:

```kira
let (sender, receiver) = channel[str](capacity: 32)

crew c:
    c.spawn(async:
        for line in read_lines("file.txt"):
            await sender.send(line)
        sender.close()
    )
    c.spawn(async:
        while let some(line) = await receiver.recv():
            process(line)
    )
```

Sending on a full channel suspends the sender (backpressure). Receiving on an empty, closed channel returns `none`.

For a value that changes over time with multiple readers, use `watch`:

```kira
let (writer, reader) = watch[config](initial: default_config())

# in one task:
writer.set(new_config)

# in another task:
await reader.changed()    # suspends until the value changes
let current = reader.get()
```

---

## Higher-Kinded Traits

Some abstractions require traits over type constructors — types that themselves take type arguments. This is an advanced feature used primarily in library design.

```kira
trait functor[F[_]]:
    def map[A, B](fa: F[A], f: fn(A) -> B) -> F[B]

trait monad[M[_]] requires functor[M]:
    def pure[A](a: A) -> M[A]
    def bind[A, B](ma: M[A], f: fn(A) -> M[B]) -> M[B]

impl monad[option]:
    def pure[A](a: A) -> option[A]: some(a)
    def bind[A, B](ma: option[A], f: fn(A) -> option[B]) -> option[B]:
        match ma:
            some(a) => f(a)
            none    => none
```

---

## The Prelude

The following are available in every module without any `use` declaration:

**Types:** `bool`, `char`, `str`, `unit`, `byte`, all numeric types, `array`, `slice`, `slice_mut`, `option`, `result`, `list`

**Traits:** `eq`, `ord`, `hash`, `show`, `from`, `into`, `add`, `sub`, `mul`, `div`, `neg`, and the other arithmetic operator traits

**Concepts:** `send`, `share`

**Functions:** `println`, `print`, `panic`, `assert`, `size_of`, `args`, `env`

To opt out of the prelude entirely (for low-level or embedded work):

```kira
module my_module
no_prelude
```

---

## Summary of Design Decisions

| Decision | Consequence |
|---|---|
| No dynamic dispatch | All polymorphism resolved at compile time; no hidden vtables |
| No exceptions | All failure paths visible in types; `?` keeps them ergonomic |
| No dynamic typing | Inference fills in types; ambiguity is a compile error |
| Modules span files; dotted paths are folders (`pub`/`module`/`file` visibility) | A module is a unit of privacy, not a file; a package exposes its `pub` surface via compile-time reflection |
| Modules are compile-time values; signatures are their types | Parameterized modules give ML-style functors in ordinary generics syntax; reflection over members generates code, replacing dynamic dispatch |
| `list[T]` in the prelude | Beginners have a useful collection immediately; `array[T,n]` is the fixed-size variant |
| Full compile-time execution | No separate macro or template language; reflection and codegen use ordinary Kira |
| Compile time is confluent — no mutable global compile-time state | Deterministic, parallel, incremental compilation; program-wide information comes from declarative queries, not registration |
| `array[T, n]` as sole built-in collection | All other collections are library types; `list` is the standard resizable sequence |
| Contracts (`pre`, `post`, `invariant`) | Preconditions and postconditions are part of the interface; statically verified when possible |
| Checked arithmetic by default | Overflow panics like a bad index, never UB; `+%`/`+|` give explicit wrap/saturate, safe and available everywhere |
| Conversions are constructor calls (`float64(x)`) | No cast operator; one `from` mechanism serves both conversions and `?`; narrowing is checked, never silent |
| Concepts | Named reusable constraint bundles; satisfied automatically; checked at use site |
| `requires` for trait dependencies | Dependencies are explicit and readable; implied bounds available to callers automatically |
| `machine` prefix | Low-level access is syntactically consistent with other function modifiers |
| `pure` prefix | Compiler-verified referential transparency; enables use in contracts and proof obligations |
| Quoting and splicing (`` ` `` and `~`) | Code generation uses ordinary Kira; no separate macro language; hygienic by default |
| Second-class borrows | `&`/`&mut` are call-scoped passing modes, never stored or returned, so there are no lifetime annotations; `slice`/`str` views cover borrowed windows |
| Structured concurrency (`crew`) | Tasks cannot outlive their crew; async leaks are impossible; spawned closures may borrow local data |
| Ownership + concurrency unified | Many-readers-or-one-writer is enforced statically for scoped sharing and by locks for `shared`; `send`/`share` concepts gate task boundaries |
