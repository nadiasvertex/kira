# Standard Library: `io`, `format`, `console`, `iter`, `collections`, `string`, `algorithm`

This document specifies the first seven standard library modules: `std.io`,
`std.format`, `std.console`, `std.iter`, `std.collections`, `std.string`,
and `std.algorithm`. It also tracks the compiler and language work the
earlier modules depend on (see "Compiler and Language Changes Required"
below) — several pieces of that earlier design do not run yet because the
compiler phase they need does not exist yet. `std.iter`, `std.collections`,
`std.string`, and `std.algorithm` are written against the language as
specified in `spec/kira-reference.md`, including `generator`/`yield` and the
`uninit[T, N]` machine primitive; nothing in these four modules is blocked.

## Design Principles

- **Thin, syscall-shaped intrinsics.** The only native surface is a handful
  of primitive operations (open/close/read/write/flush + the three standard
  streams). Everything else — buffering, formatting, the `reader`/`writer`
  trait machinery — is ordinary Kira source, shared by every backend.
- **One backend, one implementation of each intrinsic.** The bytecode
  interpreter implements intrinsics directly with C++ standard library calls
  in the VM. The AOT/LLVM backend declares each intrinsic as an external
  C-ABI symbol and links against a small native runtime library that
  implements it. Both read from one shared intrinsic schema so they cannot
  drift apart (see below).
- **Errors are data, classified once.** Intrinsics return the raw OS error
  code (`io_errno`); the Kira-side `io` module is the only place that turns
  codes into the friendly `io_error` sum type, via `impl from[io_errno] for
  io_error`. Native code never has to know about Kira's error types.
- **`show` stays the default formatter.** `std.format` does not introduce a
  second formatting mechanism — it specifies the mini-language inside `{...}`
  interpolation and lets types opt in to width/precision/fill handling via
  per-capability traits; a type that only implements `show` still works
  everywhere. The full design lives in `spec/string-formatting-design.md`.
- **Algorithms bind to the weakest capability that does the job.** Every
  function asks for the least it can get away with, and the thing it asks for
  is a *name* — `some iterator[T]` to read forward once, `some forward[T]` to
  read twice, `some bidirectional[T]` to read backwards, `some
  random_access[T]` to jump. A caller never has to work out from a function's
  body what it secretly needs; the signature says it. Mutation is the one
  capability that is *not* on that ladder: rearranging elements in place takes
  `mut slice[T]`, because "I can hand you back an element to overwrite" is a
  property of the storage, not of a cursor walking over it (see The Capability
  Ladder in `std.iter`, and `std.algorithm`).
- **No addressable view where none exists.** A collection implements
  `iterable`, or an indexing-style accessor, only where an element genuinely
  has an address a `cell[T]` can point to. `heap[T]` and `bitset[n]` both
  decline rather than fake one — see their entries in `std.collections`
  below for why a proxy-reference workaround (as in C++'s
  `vector<bool>`) is treated as a mistake to avoid, not a feature to match.

---

## Intrinsics

Eight intrinsics, all syscall-shaped. `raw_fd` is an opaque handle (not a
bare POSIX integer) so the AOT runtime is free to store a platform handle
(e.g. a Windows `HANDLE`) in it without changing the ABI shape.

```kira
intrinsic def rt_stdin()  -> raw_fd
intrinsic def rt_stdout() -> raw_fd
intrinsic def rt_stderr() -> raw_fd

intrinsic def rt_open(path: str, opts: open_options) -> result[raw_fd, io_errno]
intrinsic def rt_close(fd: raw_fd) -> result[unit, io_errno]
intrinsic def rt_read(fd: raw_fd, buf: slice_mut[byte]) -> result[usize, io_errno]
intrinsic def rt_write(fd: raw_fd, buf: slice[byte]) -> result[usize, io_errno]
intrinsic def rt_flush(fd: raw_fd) -> result[unit, io_errno]
```

An `intrinsic def` has a signature and no body. It is not part of the public
API of any stdlib module — `std.io` is the only thing that calls these
directly, and it wraps them in `reader`/`writer`/`file`. No `seek` yet; add
it once something needs random access.

### Shared intrinsic schema

Both backends (bytecode VM, LLVM/AOT) and the semantic checker's intrinsic
registry must agree on the same (name, id, signature) table. This table
should be defined exactly once — e.g. as an X-macro list — and consumed by:

- the semantic checker, to validate `intrinsic def` signatures and assign a
  stable numeric id used by the bytecode compiler,
- the bytecode VM's dispatch table (`op_call_intrinsic` indexes into an
  array of C++ function pointers),
- the LLVM codegen's `declare` list and the native runtime library's symbol
  names (e.g. `kira_rt_write`).

A hand-maintained duplicate list in each backend is exactly the kind of
drift this schema exists to prevent.

---

## `std.io`

```kira
module std.io

pub type raw_fd = { value: int64 }
pub type io_errno = { code: int32 }

pub type io_error =
    | @not_found(str)
    | @permission_denied(str)
    | @already_exists(str)
    | @interrupted
    | @would_block
    | @broken_pipe
    | @unexpected_eof
    | @other(io_errno)

impl from[io_errno] for io_error:
    def from(e: io_errno) -> io_error: ...   # maps OS codes to variants

pub type open_options = {
    read:     bool,
    write:    bool,
    append:   bool,
    create:   bool,
    truncate: bool,
}

pub trait reader:
    def read(mut self, buf: slice_mut[byte]) -> result[usize, io_error]

    def read_to_end(mut self, out: mut list[byte]) -> result[usize, io_error]:
        ...   # default, built on read()

    def read_exact(mut self, buf: slice_mut[byte]) -> result[unit, io_error]:
        ...   # default, built on read()

pub trait writer:
    def write(mut self, buf: slice[byte]) -> result[usize, io_error]
    def flush(mut self) -> result[unit, io_error]

    def write_all(mut self, buf: slice[byte]) -> result[unit, io_error]:
        ...   # default, loops write() until buf is consumed

pub type file = { fd: raw_fd }

pub def open(path: str, opts: open_options) -> result[file, io_error]: ...

impl reader for file:
    def read(mut self, buf: slice_mut[byte]) -> result[usize, io_error]:
        rt_read(self.fd, buf).map_err(io_error.from)

impl writer for file:
    def write(mut self, buf: slice[byte]) -> result[usize, io_error]:
        rt_write(self.fd, buf).map_err(io_error.from)
    def flush(mut self) -> result[unit, io_error]:
        rt_flush(self.fd).map_err(io_error.from)

impl drop for file:
    def drop(mut self) -> unit:
        _ = rt_close(self.fd)   # best-effort — a drop can't hand back an error

# offered for callers who need to *observe* a close failure
pub def (file).close(mut self) -> result[unit, io_error]:
    let r = rt_close(self.fd)
    self.fd = closed_sentinel   # drop() then no-ops instead of double-closing
    return r.map_err(io_error.from)
```

`file` relies on the `drop` trait (see `spec/kira-reference.md`, "Destructors:
`drop`") for automatic cleanup — no mandatory explicit close. The same
best-effort-drop-plus-explicit-fallible-method pattern should be reused by
any future buffered writer, since a drop-only flush that silently swallows
errors is a well-known footgun (Rust's `BufWriter` has exactly this
problem).

---

## `std.format`

Builds on the prelude's `show` trait rather than replacing it. The full
design — the `{expr:spec}` mini-language, the per-capability `show`/`debug`/
`hex`/`octal`/`binary` traits, the `format_spec` shape, and the
sign/prefix-aware padding rules — is specified in full in
`spec/string-formatting-design.md`; that document is authoritative for this
module's contents, superseding the earlier `format_with`/single-`format_spec`
sketch that used to live here.

```kira
module std.format

pub type align_mode = @left | @right | @center
pub type sign_mode   = @always | @negative_only | @space
pub type format_spec = { ... }   # see spec/string-formatting-design.md

pub def pad_str(s: str, spec: format_spec) -> str
pub def pad_integral(negative: bool, prefix: str, digits: str, spec: format_spec) -> str
```

`show`, `debug`, `hex`, `octal`, and `binary` are prelude traits (not
`std.format`-scoped) so that `impl show for T` keeps working without a
`use` of this module — `std.format` supplies the `format_spec` type and the
`pad_str`/`pad_integral` helpers the compiler's generated formatting code
calls into.

---

## `std.console`

Thin — sits entirely on `io::writer`/`io::reader`.

```kira
module std.console

use std.io.{ file, writer, reader }

pub def stdout() -> file
pub def stderr() -> file
pub def stdin()  -> file

pub def print(s: str) -> unit:    stdout().write_all(s.as_bytes())
pub def println(s: str) -> unit:  print(s); print("\n")
pub def eprint(s: str) -> unit:   stderr().write_all(s.as_bytes())
pub def eprintln(s: str) -> unit: eprint(s); eprint("\n")
```

The prelude's `print`/`println` (see `spec/kira-reference.md`, "The
Prelude") re-export these.

---

## `std.iter`

Splits what the earlier prelude examples (`make_iter() -> some iterator[int32]`)
already assumed into two traits: collections implement `iterable`, one-shot
cursors over them implement `iterator`. This is what makes "can I iterate
this twice?" a non-question — iterating a collection asks it for a fresh
`iterator`, which borrows the collection but does not consume it.

```kira
module std.iter

pub trait iterable[T]:
    def iter(self) -> some iterator[T]

pub trait iterator[T]:
    def next(mut self) -> option[T]

    # Defaults built on next() — every adapter below returns some iterator[...]
    # and can be written by hand or with a generator body; callers cannot tell
    # which. See "Generators: `generator` and `yield`" in spec/kira-reference.md.
    def map[U](self, f: fn(T) -> U) -> some iterator[U]: ...
    def filter(self, pred: fn(&T) -> bool) -> some iterator[T]: ...
    def take(self, n: usize) -> some iterator[T]: ...
    def take_while(self, pred: fn(&T) -> bool) -> some iterator[T]: ...
    def skip(self, n: usize) -> some iterator[T]: ...
    def skip_while(self, pred: fn(&T) -> bool) -> some iterator[T]: ...
    def step_by(self, n: usize) -> some iterator[T]: ...
    def chain(self, other: some iterator[T]) -> some iterator[T]: ...
    def zip[U](self, other: some iterator[U]) -> some iterator[(T, U)]: ...
    def enumerate(self) -> some iterator[(usize, T)]: ...
    def peekable(self) -> some peekable[T]: ...
    def scan[S, U](self, init: S, f: fn(mut S, T) -> option[U]) -> some iterator[U]: ...

    def fold[A](self, init: A, f: fn(A, T) -> A) -> A: ...
    def find(self, pred: fn(&T) -> bool) -> option[T]: ...
    def count(self) -> usize: ...
    def any(self, pred: fn(&T) -> bool) -> bool: ...
    def all(self, pred: fn(&T) -> bool) -> bool: ...

    def collect(self) -> list[T]: ...
        # the common target; other collections provide their own constructor —
        # see map_from_iter / set_from_iter in std.collections below.

pub trait peekable[T] requires iterator[T]:
    def peek(self) -> option[cell[T]]
```

### `filter` and `map`, written by hand vs. generated

Every adapter above has the same shape: hold a base iterator (and, for
`filter`/`scan`, a little extra state), and drive it from `next()`. Written
by hand, `filter` is a small state machine:

```kira
type filter_iter[T] = { base: some iterator[T], pred: fn(&T) -> bool }

impl iterator[T] for filter_iter[T]:
    def next(mut self) -> option[T]:
        while let @some(x) = self.base.next():
            if self.pred(&x): return @some(x)
        return @none
```

Written as a generator, it is the loop you would describe out loud, with the
state machine produced by the compiler:

```kira
generator def filter[T](base: some iterator[T], pred: fn(&T) -> bool) -> some iterator[T]:
    for x in base:
        if pred(&x): yield x
```

Both compile to the same thing and both satisfy `iterator[T]`; the stdlib
uses the generator form throughout because most adapters here read exactly
like their informal description (`zip`, `chain`, `enumerate`, `step_by`, and
the windowing adapters below all follow the same pattern).

### The capability ladder

`iterator[T]` alone gives every algorithm that only needs to read forward
once. Multi-pass, bidirectional, and random-access algorithms need more, and
each level is a `requires` step up from the one below (see `requires` —
Trait Dependencies):

```kira
pub trait forward[T] requires iterator[T]:
    def save(self) -> some forward[T]
        # a cheap, independent cursor at the current position — the base
        # iterator can keep advancing without disturbing a saved one

pub trait bidirectional[T] requires forward[T]:
    def next_back(mut self) -> option[T]

    def rev(self) -> some iterator[T]: ...   # default, built on next_back()

pub trait exact_size[T] requires iterator[T]:
    def remaining(self) -> usize
        # how many elements are still to come, known without walking them

pub trait random_access[T] requires bidirectional[T] + exact_size[T]:
    def skip_to(mut self, n: usize) -> unit
```

`exact_size` is a rung of its own rather than a member of `random_access`
because the two capabilities genuinely come apart: `map[K, V]` knows exactly
how many entries it will produce and can say so in O(1), but it cannot jump to
the 400th one. `random_access` requires it (a cursor that can jump certainly
knows how far it has left to go, which is why it needs no separate `len()`),
but plenty of sized things are not jumpable.

Its payoff is `collect()`. That default is written to ask, at compile time,
whether the concrete iterator it was monomorphized for satisfies `exact_size`;
when it does, it allocates `remaining()` elements once instead of growing the
list as it goes. Nothing about the call changes — `xs.cursor().map(f).collect()`
is spelled the same either way — but `list`, `slice`, `deque`, and `map`
sources land in the pre-sized path, while `filter` and `take_while`, whose
output length genuinely is not knowable up front, land in the growing one and
are right to.

### `iter()` and `cursor()`

A trait method's return type is fixed by the trait. `iterable[T]` declares
`iter(self) -> some iterator[T]`, and an `impl` supplies some concrete type
satisfying `iterator[T]` — but it cannot *widen the promise the trait made to
its callers*, because generic code bounded by `[C: iterable[T]]` was compiled
against `iterator[T]` and can only ever see that much. So `iter()` is, and
stays, the weakest rung.

Collections that can do better publish it under a second name. `cursor()` is
that name, and it always means "the strongest cursor this type can give":

```kira
extend slice[T]:
    def cursor(self) -> some random_access[T]: ...

extend list[T]:
    def cursor(self) -> some random_access[T]: self.as_slice().cursor()

extend array[T, n]:
    def cursor(self) -> some random_access[T]: self.as_slice().cursor()
```

These are extensions on prelude types, which needs no coherence exception and
no language change (see Extensions in `spec/kira-reference.md`): an extension
makes no global claim, so `std.iter` may attach `cursor()` to `slice[T]`
freely, and it is visible wherever `std.iter` is `use`d.

The split is the same one Rust draws between an inherent `Vec::iter` and the
`IntoIterator` impl, and this document already had a case of it before the
capability ladder was named: `btree_map.range()` returns `some
bidirectional[...]` precisely because the `iterable` impl could not. The rule
for a reader is short — **`for` loops and `iterable`-generic code take
`iter()`; an algorithm that needs a rung above `iterator` asks for
`cursor()`** — and the compiler enforces it, since passing `xs.iter()` where
`some random_access[T]` was wanted is an error naming the rung that is missing.

(Collapsing the two would need `iterable` to carry a *bounded* associated
cursor type — `type cursor: iterator[T]`, with impls free to satisfy it with
something stronger. The grammar's `associated_type_decl` has no bound today;
it is logged under Compiler and Language Changes Required below, and it is a
simplification, not a blocker.)

### Which types have which capability

You should never have to read a collection's implementation to find out what it
can do. Every type's strongest cursor, and the accessor that yields it:

| Source | Strongest cursor | Reached by | Why that rung |
|---|---|---|---|
| `slice[T]`, `list[T]`, `array[T, n]` | `random_access` | `cursor()` | contiguously backed |
| `deque[T]` | `random_access` | `cursor()` | ring buffer — indexable, though not contiguous |
| `btree_map`, `btree_set` | `bidirectional` | `range()` | ordered tree cursor: walks both ways, can't jump |
| `map[K, V]`, `set[T]` | `exact_size` | `keys()`, `values()`, `entries()` | hash order: the count is known, a position is not |
| `bitset[n]` | `iterator` | `iter_ones()` | word-skipping scan, one pass |
| `heap[T]` | — | — | heap order is not a meaningful sequence; see `std.collections` |
| generators (`generator def`) | `iterator` | the generator call itself | a suspended function has no cursor to save |

Every row that has a cursor at all also answers `iter()` at the `iterator` rung,
via `iterable` — that is what `for x in xs` uses, and it is all a function
bounded by `[C: iterable[T]]` can rely on.

The adapters preserve what they can: `rev`, `enumerate`, and `map` keep
whatever the base had; `filter`, `take_while`, and `scan` drop to plain
`iterator`, because none of them can say how many elements survive without
running the predicate. This is not a limitation to route around — it is the
ladder reporting the truth. `xs.cursor().filter(p)` really has lost random
access, and an algorithm that then demands it is right to reject the result.

`rev()` needs `bidirectional`; `windows`/`chunks`/predicate-based grouping
need `forward`, because producing one window means holding a saved cursor
while the base iterator keeps moving past it:

```kira
generator def windows[T](base: some forward[T], size: usize) -> some iterator[list[T]]:
    var cursor = base
    loop:
        var peek = cursor.save()      # walks size elements without disturbing cursor
        var w: list[T] = []
        for _ in 0..size:
            match peek.next():
                @some(x) => w.push(x)
                @none    => return
        yield w
        match cursor.next():          # slide the real cursor forward by exactly one
            @some(_) => ()
            @none    => return
```

`cycle` is a different requirement entirely — it does not need to *rewind*
an iterator, it needs to *restart* one, which means it needs the `iterable`
collection, not a `forward` cursor over it:

```kira
generator def cycle[T](src: some iterable[T]) -> some iterator[T]:
    loop:
        for x in src.iter():
            yield x
```

This is the practical payoff of splitting `iterable` from `iterator` in the
first place: "needs to look back" (`forward`) and "needs to start over"
(`iterable`) are different requirements, and the type system lets each
adapter ask for exactly the one it needs instead of a single vague
"replayable" bound that would force every adapter to pay for both.

`tee` — fanning one iterator into two independently-advanceable ones — takes
`forward` on the source when available, and otherwise buffers internally so
the faster of the two consumers doesn't wait on the slower one; document
which you have at the call site, the same tradeoff other range libraries
make rather than solving away.

### Unbounded sources

Paired with `take`/`take_while`, an iterator that never returns `@none` on
its own is a normal, useful thing to build with a generator:

```kira
generator def naturals() -> some iterator[uint64]:
    var n: uint64 = 0
    loop:
        yield n
        n += 1

let first_five = naturals().take(5).collect()   # [0, 1, 2, 3, 4]
```

An open-ended range (`0..`, see `for` in `spec/kira-reference.md`) is the
common case of this and is `iterable` the same way `0..10` is.

---

## `std.collections`

`list[T]` and `array[T, n]` are in the prelude (see Collections in
`spec/kira-reference.md`) and are not repeated here. This module covers
everything else, organized by the same keep/change/skip/add reasoning that
motivated it: match a well-understood C++/Rust shape where one earns its
keep, change the representation where a better one is available for free,
and skip a type entirely rather than pad the surface with something a
two-line composition already covers.

```kira
module std.collections

use std.iter.{ iterable, iterator, forward, bidirectional, random_access, exact_size }

pub concept key[K]:
    K: hash + eq
```

`key` is the one constraint bundle this library names. `map`, `set`, and both
`*_from_iter` constructors all require exactly `hash + eq` and always will —
the two are meaningless apart, since a hash table needs to bucket a key *and*
to confirm a hit within the bucket. Naming it means the error a user sees when
they hand a `map` an unhashable key is one the compiler can explain in the
library's vocabulary ("`point` cannot be a `key`: it implements `eq` but not
`hash` — add `deriving hash`") rather than restating a bound.

That is also why it is the *only* concept here. A concept earns its name by
bundling constraints that recur together; almost every other bound in this
library is a single trait (`[T: ord]`, `[T: eq]`, `[T: send]`), and wrapping
one trait in a concept adds a layer of indirection a reader has to unwrap for
nothing. Concepts are how a generic function states what it requires — they do
not multiply just because a library is large.

### `deque[T]`

A ring-buffer-backed double-ended queue — push/pop at either end in O(1)
amortized. The obvious fit for BFS and sliding-window problems, and cheap
enough that there's no reason to fake it out of two `list[T]`s.

```kira
pub type deque[T] = { buf: list[T], head: usize, len: usize }

extend deque[T]:
    def push_front(mut self, x: T) -> unit: ...
    def push_back(mut self, x: T) -> unit: ...
    def pop_front(mut self) -> option[T]: ...
    def pop_back(mut self) -> option[T]: ...

extend deque[T]:
    def cursor(self) -> some random_access[T]: ...

impl iterable[T] for deque[T]:
    def iter(self) -> some iterator[T]: ...
```

`deque[T]` follows the `iter()`/`cursor()` convention from `std.iter`: `iter()`
for `for` loops and `iterable`-generic code, `cursor()` for the strongest thing
it has. And a ring buffer's strongest thing is `random_access` — element *i*
lives at `(head + i) % buf.len()`, so it is indexable even though it is not
contiguous and has no `slice[T]` to hand out.

That combination is what makes the ladder pay for itself. Every read-only
positional algorithm in `std.algorithm` (`binary_search` and the bound-finding
family) binds to `some random_access[T]` rather than to `slice[T]`, so all of
them accept `d.cursor()` and work on a sorted `deque[T]` directly. Binding them
to `slice[T]` — the obvious shortcut, since a slice *has* random access — would
have forced a `deque` to copy itself into a `list` before it could be searched,
paying an allocation and a full element copy to prove a property it already
had. The capability was the thing the algorithm needed; contiguity was never
more than one way to supply it.

`list[T]` already *is* a stack (`push`/`pop` at the back) and `deque[T]`
already *is* a queue (`push_back`/`pop_front`); neither gets a name-only
wrapper type the way C++'s `std::stack`/`std::queue` adapt `deque` — the
extra ceremony buys nothing.

### `heap[T]`

A binary, array-backed max-heap.

```kira
pub type heap[T: ord] = { data: list[T] }

extend heap[T: ord]:
    def push(mut self, x: T) -> unit: ...
    def pop(mut self) -> option[T]: ...
    def peek(self) -> option[cell[T]]: ...
```

`heap[T]` deliberately does **not** implement `iterable[T]`. The backing
array is heap-ordered, not sorted, so iterating it would produce a sequence
that looks meaningful and isn't — the same call C++'s `priority_queue`
already makes. Want sorted access instead? `heap.pop()` in a loop, or
`btree_set[T]` below.

### `small_list[T, N]`

Inline storage for up to `N` elements — living in the struct itself, no heap
allocation — spilling to a heap `list[T]` only past that. LLVM's
`SmallVector` and Abseil's `InlinedVector` in one first-party type, made
possible by `array[T, n]`'s dependent sizing and the `uninit[T, N]` machine
primitive (see The `machine` Layer in `spec/kira-reference.md`).

```kira
type small_list[T, N: usize] =
    @inline({ buf: uninit[T, N], len: usize })
    | @spilled(list[T])

extend small_list[T, N]:
    def push(mut self, x: T) -> unit:
        match self:
            @inline(s) if s.len < N =>
                write_slot(&mut s.buf, s.len, x)
                s.len += 1
            @inline(s) =>
                var spilled: list[T] = list.with_capacity(N * 2)
                for i in 0..N: spilled.push(read_slot(&mut s.buf, i))
                spilled.push(x)
                self = @spilled(spilled)
            @spilled(l) =>
                l.push(x)

    def as_slice(self) -> slice[T]:
        match self:
            @inline(s) => as_slice(&s.buf, s.len)
            @spilled(l) => l.as_slice()

impl drop for small_list[T, N]:
    def drop(mut self) -> unit:
        match self:
            @inline(s)  => drop_first(&mut s.buf, s.len)
            @spilled(_) => ()   # the payload's own drop already covers it
```

`push`, `as_slice`, and `drop` are ordinary functions — none of them carries
the `machine` prefix itself. The unsafety is contained entirely inside
`write_slot`/`read_slot`/`drop_first`/`as_slice` on `uninit[T, N]` (see The
`machine` Layer); `small_list[T, N]` calls those once, internally, and
everyone else gets a fully safe type.

`as_slice` looks like it needs a special borrow rule for "the view points to
different memory depending on a runtime tag." It doesn't: both match arms
return `slice[T]` derived from `self`, and view inference is stated in terms
of the argument a view is derived from (see Views: Slices and Strings), not
in terms of which field the bytes happen to live in. One inferred borrow of
`self`, two implementations underneath it. This also means `small_list`
needs no `Pin`-style escape hatch for its self-referential inline case —
unlike a raw pointer stored as a field, the pointer into `self`'s own bytes
only ever exists as the *returned* slice, which is exactly the view
machinery already preventing `self` from moving while it's borrowed.

Once spilled, a `small_list` never moves back to inline storage — one
direction, same policy as its Rust and LLVM counterparts. `small_list[T, 0]`
is legal and degenerates to always-spilled, the same as `array[T, 0]` being
legal-but-unused today; neither needs a static ban.

### `bitset[n]`

A packed bit set, sized entirely by a dependent type:

```kira
type bitset[n: usize] = { words: array[uint64, (n + 63) / 64] }
    invariant padding_bits_zero(self)

pure def word_count(n: usize) -> usize: (n + 63) / 64
pure def last_word_index(n: usize) -> usize: word_count(n) - 1

pure def padding_bits_zero[n: usize](b: bitset[n]) -> bool:
    n % 64 == 0 or (b.words[last_word_index(n)] >> (n % 64)) == 0
```

`(n + 63) / 64` is `ceil(n / 64)`, the word count, computed in type position
the same way `concat`'s `vec[T, m + n]` computes a length (see Dependent
Types). The `invariant` catches the classic packed-bitset bug at its
source: any operation that can set a padding bit above position `n - 1` —
`bitnot` above all, since complementing a word also complements its unused
high bits — must re-mask before returning, or `count_ones`/`all`/equality
silently go wrong. With the invariant in place, forgetting the mask is a
panic naming `bitnot`, not a mystery three call sites downstream.

```kira
extend bitset[n]:
    def get(self, i: index[n]) -> bool:
        let (w, b) = split(i)
        (self.words[w] >> b) & 1 == 1

    def set(mut self, i: index[n], v: bool) -> unit:
        let (w, b) = split(i)
        if v: self.words[w] |= (uint64(1) << b)
        else: self.words[w] &= ~(uint64(1) << b)

    def toggle(mut self, i: index[n]) -> unit: ...
    def count_ones(self) -> usize: ...
    def any(self) -> bool: self.count_ones() > 0
    def none(self) -> bool: not self.any()
    def all(self) -> bool: self.count_ones() == n

    def iter_bits(self) -> some iterator[bool]: ...   # every position, by value
    def iter_ones(self) -> some iterator[usize]: ...  # positions of set bits, word-skipping

def split[n: usize](i: index[n]) -> (index[word_count(n)], uint8):
    let wi = i / 64
    match index[word_count(n)].try_from(wi):
        @some(w) => (w, uint8(i % 64))
        @none    => panic("unreachable if split is correct")   # see below

impl bitand for bitset[n]:
    type output = bitset[n]
    def bitand(self, other: self) -> self.output: ...   # word-wise, no re-masking needed
impl bitor for bitset[n]:
    type output = bitset[n]
    def bitor(self, other: self) -> self.output: ...
impl bitxor for bitset[n]:
    type output = bitset[n]
    def bitxor(self, other: self) -> self.output: ...
impl bitnot for bitset[n]:
    type output = bitset[n]
    def bitnot(self) -> self.output: ...   # must re-mask the last word — see invariant above
```

(`bitand`/`bitor`/`bitxor`/`bitnot` follow the same `type output` shape as `add` in Operator Overloading; they are the bitwise members of "the other arithmetic operator traits" the prelude already provides for the built-in integer types, implemented here for `bitset[n]` the same way `impl add for vec2` was shown.)

`i / 64 < word_count(n)` given `i < n` is true, but it takes a step of
reasoning about integer division the constraint solver may or may not
discharge on its own (see Refinement Types). `split` is written to hold
either way: `index[...].try_from` is the documented fallback for exactly
this case — a compile-time-eliminated check if the solver proves it, a
cheap runtime check if it doesn't, correct either way.

`bitset[n]` implements neither an addressable `get`/`set` via `cell[bool]`
nor `iterable[bool]`. A single bit inside a packed word has no address a
`cell[T]` could point to — this is the same shape of problem C++'s
`vector<bool>` ran into with its proxy `reference` type, and the fix here is
structural rather than clever: `get`/`set` pass `bool` by value, and
`iter_ones` returns *positions* rather than element handles. `bitset[n]`
declining `iterable[bool]` — like `heap[T]` declining `iterable[T]` for its
own, different reason — is exactly why `iterable` is a trait a type opts
into rather than something every collection-shaped type is forced to
satisfy: forced conformance is what pushed `vector<bool>` into faking
addressability it didn't have.

### `map[K, V]` and `set[T]`

Hash-based, open-addressed. `iter()` (and therefore `iterable`) is built
over the map's own backing storage, not exposed as public API:

```kira
type slot[K, V] = @empty | @occupied(K, V) | @tombstone
pub type map[K: key, V] = { slots: list[slot[K, V]], len: usize }

impl iterable[(K, V)] for map[K, V]:
    generator def iter(self) -> some iterator[(cell[K], cell[V])]:
        for i in 0..self.slots.len():
            if let @occupied(k, v) = &self.slots[i]:
                yield (cell(k), cell(v))

extend map[K: key, V]:
    def keys(self) -> some exact_size[cell[K]]: ...
    def values(self) -> some exact_size[cell[V]]: ...
    def entries(self) -> some exact_size[(cell[K], cell[V])]: ...
    def get_mut(mut self, k: &K) -> option[mut cell[V]]: ...
    def insert(mut self, k: K, v: V) -> option[V]: ...

pub def map_from_iter[K: key, V](it: some iterator[(K, V)]) -> map[K, V]: ...

pub type set[T: key] = { inner: map[T, unit] }
pub def set_from_iter[T: key](it: some iterator[T]) -> set[T]: ...
```

`keys`/`values`/`entries` return `some exact_size[...]` — the same
inherent-method-alongside-the-impl shape as `deque.cursor()` above. A map knows
its own `len` without walking its slots, so `m.values().collect()` (the
documented way to get an owned `list[V]` out of a map) sizes its list once up
front rather than doubling its way there. Hash order still isn't a *position*,
so these cursors stop at `exact_size` and never claim `forward` or above:
knowing how many are left and being able to jump to the *n*th are different
powers, and a map has exactly the first.

`iter()`'s generator body reaches `self.slots` across every suspension at
`yield`, so the state the compiler synthesizes for it captures a borrow of
`self` the same way a hand-written struct with a `slice[slot[K,V]]` field
would — the view-through-structs rule from `spec/kira-reference.md` applies
to a compiler-generated coroutine state exactly as it does to one you write
by hand. `iter()` therefore keeps the map borrowed for as long as the
returned iterator lives, so `insert` — which may trigger a rehash and
reallocate `slots` — cannot be called while an iterator is outstanding.
That's a compile error, not the dangling iterator C++ gives you or the
runtime exception Python gives you.

There is no `map[K, V].slice()`, and none is planned. A map's backing order
is hash order, not a stable, user-meaningful sequence — giving it a
`slice`-shaped API would promise an ordering guarantee the representation
doesn't have. `map.values().collect() : list[V]` is the correct way to get
a genuinely contiguous, owned copy when one is needed; that's also the
right place to pay for it, since "a live contiguous view of a hash table's
values" isn't coherent regardless of language.

There is deliberately no `multimap`/`multiset` type — `map[K, list[V]]`
covers it in two lines, and a dedicated type would only double the API
surface for no added capability.

### `btree_map[K, V]` and `btree_set[T]`

The ordered option, B-tree backed rather than red-black-tree backed —
better cache locality than a pointer-chasing node tree for the same
asymptotic guarantees, and the natural fit for a range query:

```kira
pub type btree_map[K: ord, V] = { ... }   # B-tree, node fanout implementation-defined

extend btree_map[K: ord, V]:
    def range(self, r: range_bounds[K]) -> some bidirectional[(cell[K], cell[V])]: ...
```

The cursor `range` returns is a second, tree-shaped analog of `slice[T]`:
not contiguous, but still a view over one collection's private storage,
covered by the same borrow inference as any other view-carrying struct. No
red-black variant is offered alongside it — a B-tree wins on locality with
no asymptotic tradeoff, so there is no reason to maintain two ordered map
implementations.

---

## `std.string`

The one gap the earlier `std.io`/`std.console` design left open: `str` (see
Strings in `spec/kira-reference.md`) is an immutable UTF-8 *view*, and there
was no owned, growable string to build one up in a loop. `string` is that
type — `list[byte]` with a UTF-8-validity invariant maintained on every
mutation, not a bare alias:

```kira
module std.string

use std.io.{ writer, io_error }

pub type string = { bytes: list[byte] }
    invariant is_valid_utf8(self.bytes.as_slice())

impl from[str] for string:
    def from(s: str) -> string: string{ bytes: s.as_bytes().to_list() }

extend string:
    def push(mut self, c: char) -> unit: ...
    def push_str(mut self, s: str) -> unit: ...
    def as_str(self) -> str: ...          # a view — see Views: Slices and Strings

impl writer for string:
    def write(mut self, buf: slice[byte]) -> result[usize, io_error]: ...
    def flush(mut self) -> result[unit, io_error]: @ok(())
```

`impl from[str] for string` is what makes `string("hello")` construct one and
`string("")` the empty case — the same call-the-type-like-a-constructor
convention as `float64(n)` (see Converting Between Types), not a bespoke
`new()`/`from()` pair.

Implementing `writer` is the point: it's what lets `std.format`'s generated
formatting code build directly into a `string` (`write!`-style usage)
instead of allocating an intermediate value per interpolated piece — the
single biggest consumer of a mutable string type is exactly the formatting
work `std.format` already specifies.

---

## `std.algorithm`

Every function here binds to the weakest capability that does its job, and
names that capability in its signature. There are two axes, and they are not
the same axis:

- **How much of the sequence a function needs to see** — one forward pass
  (`some iterator[T]`), two (`some forward[T]`), backwards (`some
  bidirectional[T]`), or by position (`some random_access[T]`). These are the
  rungs of the ladder in `std.iter`, and an algorithm asks for a rung.
- **Whether it writes** — in-place rearrangement takes `mut slice[T]`, and
  nothing on the ladder above will do. Handing back an element to overwrite is
  a property of *storage*, not of a cursor: it is the thing C++ needed output
  iterators and proxy references for, and the reason `vector<bool>` broke. A
  `mut slice[T]` is honest about it. `sort` therefore takes `mut slice[T]`, not
  `mut random_access[T]`, and there is no such trait to take.

So the read-only algorithms follow the ladder and the mutating ones follow the
storage, and each says which in its own signature. A caller reading `def
binary_search[T: ord](s: some random_access[T], ...)` knows what to bring
without knowing how binary search is implemented, and `deque[T]` — indexable,
but with no `slice` to offer — gets to use it.

```kira
module std.algorithm

use std.iter.{ iterator, forward, bidirectional, random_access }
```

### Reading — `some iterator[T]`

```kira
pub def find[T](it: some iterator[T], pred: fn(&T) -> bool) -> option[T]: ...
pub def count[T](it: some iterator[T], pred: fn(&T) -> bool) -> usize: ...
pub def all_of[T](it: some iterator[T], pred: fn(&T) -> bool) -> bool: ...
pub def any_of[T](it: some iterator[T], pred: fn(&T) -> bool) -> bool: ...
pub def none_of[T](it: some iterator[T], pred: fn(&T) -> bool) -> bool: ...
pub def equal[T: eq](a: some iterator[T], b: some iterator[T]) -> bool: ...
pub def mismatch[T: eq](a: some iterator[T], b: some iterator[T]) -> option[(T, T)]: ...
pub def adjacent_find[T: eq](it: some iterator[T]) -> option[T]: ...
pub def accumulate[T, A](it: some iterator[T], init: A, f: fn(A, T) -> A) -> A: ...
pub def min_element[T: ord](it: some iterator[T]) -> option[T]: ...
pub def max_element[T: ord](it: some iterator[T]) -> option[T]: ...
```

There is no `for_each`. `for x in xs: ...` already reads better than
threading a lambda through a call, and unlike C++ circa 1998, `for` was
never missing to begin with.

### Reading more than once — `some forward[T]`

Finding a *subsequence* is not a single-pass problem: a failed match at one
position has to resume from the element after where the attempt started, not
from where it gave up. That is `save()`, and so these take `some forward[T]`:

```kira
pub def search[T: eq](haystack: some forward[T], needle: some forward[T]) -> option[usize]: ...
pub def find_end[T: eq](haystack: some forward[T], needle: some forward[T]) -> option[usize]: ...
pub def is_sorted[T: ord](it: some forward[T]) -> bool: ...
```

`is_sorted` is on this rung for a subtler reason, and it is the reason the
`pre` contracts below can exist at all. Checking sortedness by walking a
one-pass iterator would *consume the very sequence the caller was about to
use*, so a precondition written against `some iterator[T]` could never be
anything but a lie. Against `some forward[T]` it costs a `save()` and leaves
the caller's cursor exactly where it was.

### Reading backwards — `some bidirectional[T]`

```kira
pub def rfind[T](it: some bidirectional[T], pred: fn(&T) -> bool) -> option[T]: ...
pub def ends_with[T: eq](it: some bidirectional[T], suffix: some bidirectional[T]) -> bool: ...
```

Searching from the back is one `next_back()` call away when the source can do
it, and requires buffering the entire sequence when it can't — so it asks, and
a caller who only has a one-pass iterator gets a compile error naming
`bidirectional` instead of quietly paying for a full copy.

### Rearranging in place — `mut slice[T]`

```kira
pub def sort[T: ord](s: mut slice[T]) -> unit: ...
pub def stable_sort[T: ord](s: mut slice[T]) -> unit: ...
pub def partial_sort[T: ord](s: mut slice[T], mid: usize) -> unit: ...
pub def nth_element[T: ord](s: mut slice[T], n: usize) -> unit: ...
pub def reverse[T](s: mut slice[T]) -> unit: ...
pub def rotate[T](s: mut slice[T], mid: usize) -> unit: ...
pub def partition[T](s: mut slice[T], pred: fn(&T) -> bool) -> usize: ...
pub def stable_partition[T](s: mut slice[T], pred: fn(&T) -> bool) -> usize: ...
pub def unique[T: eq](s: mut slice[T]) -> usize: ...   # dedups adjacent runs, returns new length
pub def next_permutation[T: ord](s: mut slice[T]) -> bool: ...
pub def shuffle[T](s: mut slice[T], rng: mut rng_t) -> unit: ...

pub def make_heap[T: ord](s: mut slice[T]) -> unit: ...
pub def push_heap[T: ord](s: mut slice[T]) -> unit: ...
pub def pop_heap[T: ord](s: mut slice[T]) -> unit: ...
pub def is_heap[T: ord](s: slice[T]) -> bool: ...
```

The heap functions above operate on a `mut slice[T]` you don't necessarily
own outright — top-k over a fixed buffer, say — which is a distinct,
legitimate use case from owning a `heap[T]` (`std.collections`) outright;
both are kept.

There is no `reverse_copy`, `rotate_copy`, `remove_copy_if`,
`replace_copy_if`, `unique_copy`, or `partition_copy`. That entire `_copy`
family exists in C++ because an output iterator is the only way to produce
a transformed sequence without allocating an intermediate — and `std.iter`
already has lazy adapters that do this directly:

```kira
let r = xs.iter().rev().collect()                    # reverse_copy
let f = xs.iter().filter(x => keep(&x)).collect()     # remove_copy_if, inverted
```

### Read-only random access, with a contract — `some random_access[T]`

Halving a search interval means jumping to the middle, which is `skip_to` —
the one thing the top rung of the ladder adds:

```kira
pub def binary_search[T: ord](s: some random_access[T], target: &T) -> option[usize]
    pre is_sorted(s), "binary_search requires a sorted sequence":
    ...

pub def lower_bound[T: ord](s: some random_access[T], target: &T) -> usize
    pre is_sorted(s), "lower_bound requires a sorted sequence":
    ...

pub def upper_bound[T: ord](s: some random_access[T], target: &T) -> usize
    pre is_sorted(s), "upper_bound requires a sorted sequence":
    ...

pub def equal_range[T: ord](s: some random_access[T], target: &T) -> (usize, usize)
    pre is_sorted(s), "equal_range requires a sorted sequence":
    ...
```

Binding to the capability rather than to `slice[T]` is what lets a sorted
`deque[T]` be searched without first being copied into contiguous memory.
`cursor()` on a `list`, an `array`, a `slice`, or a `deque` all yield
`random_access`, so one signature serves every one of them:

```kira
let i = binary_search(names.cursor(), &target)   # names: list[str] — or deque[str]
```

The `pre` contracts are the second half of the argument for the ladder. Each
one calls `is_sorted`, which needs `forward` — and `random_access` requires
`bidirectional`, which requires `forward`, so the contract is checkable on
exactly the sequences these functions accept, using `save()` to leave the
caller's cursor undisturbed. A precondition like this simply cannot be written
against `some iterator[T]`: checking it would consume the sequence the caller
was about to search. Calling any of these on an unsorted sequence is therefore
a debug-time panic naming the precondition, not the silently wrong index C++
returns in release and the undefined behavior it invites in principle.

### Parallel

```kira
pub def par_reduce[T: send](s: slice[T], f: fn(T, T) -> T) -> option[T]: ...
pub def par_sort[T: ord + send](s: mut slice[T]) -> unit: ...
pub def par_for_each[T: share](s: slice[T], f: fn(&T) -> unit) -> unit: ...
```

The three bounds differ, and the difference is the whole point. `par_reduce`
and `par_sort` *move* elements between threads — a reduction hands partial
results across, a parallel sort merges chunks owned by different threads — so
they require `send`. `par_for_each` moves nothing: it hands each worker a `&T`
into a slice that stays put, which means `T` must be safe to *observe from
several threads at once*, and that is `share`, not `send`. The two are not
interchangeable, and a type can satisfy one without the other (see Data-Race
Freedom).

These are real, checked bounds, not unchecked assertions. Where C++'s
`std::execution::par` trusts that the caller's callable is thread-safe with
nothing verifying it, handing `par_for_each` a type that isn't `share` is a
compile error naming the concept and explaining which way the data was about
to cross a thread boundary — the same check, in the same words, as at any
other task boundary.

---

## Compiler and Language Changes Required

`std.format` still cannot, entirely blocked on `{expr:spec}` interpolation splitting. This is the 
checklist of compiler work it depends on, grouped by phase.

### Semantic analysis

- [ ] **Blocked:** automatically invoking `drop()` at scope exit requires
      knowing whether a binding was moved from, which requires move/ownership
      tracking. The roadmap's "Missing" list already calls out that borrow
      checking and full move tracking do not exist yet
      (`spec/llm-compiler-roadmap.md`). Scope-exit `drop` calls should not be
      implemented ahead of that tracking — doing so risks either double-drop
      or use-after-drop bugs that look like compiler bugs to users, which
      cuts directly against the "compiler is a teacher" project goal.
- [ ] `{expr:spec}` interpolation splitting: today the lexer emits the whole
      string as one token and nothing downstream splits it
      (`src/parser/text_escape.h`); the format-spec mini-language depends
      on this splitting existing. See `spec/string-formatting-design.md`
      ("Implementation Staging") for the recommended v1 approach — a direct
      compiler pass rather than waiting on the quote/splice interpreter.

### Optional: bounded associated types

- [ ] **Not a blocker — a simplification.** `associated_type_decl`
      (`spec/kira-grammar.ebnf`) permits `type IDENT [= type_expr]` with no
      bound, so a trait cannot say "my impls must pick a cursor type, and it
      must at least satisfy `iterator[T]`". That is why `std.iter` splits
      `iter()` (fixed by `iterable` at the `iterator` rung) from `cursor()`
      (inherent, as strong as the collection can manage). Allowing a bound —
      `type cursor: iterator[T]` — plus `where`-clause refinement at use sites
      (`where C.cursor: random_access[T]`) would let a single `iter()` carry
      each collection's true capability and retire `cursor()`. Everything in
      this document works without it; nothing in it should be redesigned around
      the assumption that it will arrive.

### Running the stdlib: HIR lowering gaps

- [ ] `std.format` remains entirely blocked on `{expr:spec}` interpolation
      splitting (unchanged from above) — this is lexer/parser work, not
      lowering work, and hasn't been started.
