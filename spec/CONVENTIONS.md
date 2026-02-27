# C++ Conventions (Clang 22, C++26 mode)

> **Safety first, then readability, then performance.** Prefer functional style and value semantics. All **type** and **function** names are **snake_case**.

## 0) Toolchain & Dialect
- Target **Clang 22.1+** with **`-std=c++2c`**.
- You may use C++26 features implemented in Clang ≤ 22 (gate anything uncertain with feature‑test macros).

## 1) Naming, Layout & Structure
- All type and function names use **snake_case**. Namespaces are lower_snake_case.
- One logical public entity per header; keep headers minimal; prefer forward declares.
- Limit lines to ~100 cols; 2‑space indent; trailing commas in multi‑line init‑lists.

## 2) Functional‑first design
- Prefer pure free functions with explicit inputs/outputs.
- Prefer value semantics and immutability; use `const`/`constexpr` aggressively.
- Use `[[nodiscard]]` where ignoring a return would be a bug.

## 3) Error handling
- Use `std::expected<T, E>` for recoverable errors; `std::optional<T>` for "maybe" values.
- Reserve exceptions for truly exceptional, non‑local failures. Document them.

## 4) Value categories, moves & forwarding
- Use `std::move(x)` **only** when consuming a **named** object in the current scope.
- Use `std::forward<T>(x)` **only** in functions taking `T&&` where `T` is **deduced** (a forwarding reference).
- Prefer plain `return obj;` and let copy elision/NRVO do the work; never `return std::move(obj);` for local `obj`.

## 5) Modern features (when supported by our Clang)
- Ranges & algorithms; `[[no_unique_address]]` for EBO/layout wins (only when measured).
- Template QoL: variadic friends, pack indexing, structured‑binding in conditions, etc. (feature‑test gate).

## 6) API surface & readability
- Use `auto f(args) -> return_type` form; make ownership & mutability explicit.
- Pass cheap scalars by value; large read‑only by `const&`; sink parameters by forwarding refs + perfect forwarding.
- Return values (not output params). Use tuples or small records for multi‑values.

## 7) Memory & lifetime
- No raw owning pointers. Prefer RAII (`std::unique_ptr`, `std::shared_ptr` when required).
- Prefer `std::span`/views for non‑owning access. No naked `new`/`delete` in user code.

## 8) Concurrency
- Prefer `std::jthread` + `std::stop_token`. Avoid data races by defaulting to immutability.

## 9) Diagnostics & build flags
- Build warning‑free with at least:
  ```
  -std=c++2c -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Wdeprecated -Wold-style-cast -Werror
  ```
- Use sanitizers (`-fsanitize=address,undefined`) in CI.

## 10) Performance (after safety & clarity)
- Prefer clear, idiomatic code; then measure.
- Use `reserve`, `shrink_to_fit`, string_view/span, and rely on copy elision for return‑by‑value.

## 11) Examples

### Forwarding factory
```cpp
template <class T>
auto make_resource(T&& t) {
  return std::make_unique<std::decay_t<T>>(std::forward<T>(t));
}
```

### Return by value (let NRVO/prvalue elision work)
```cpp
auto compute_stats(std::string_view path) -> file_stats {
  file_stats s{/*...*/};
  return s; // not std::move(s)
}
```

---
**One‑liner for code‑gen**: target Clang 22.1 with `-std=c++2c`; functional‑first; type & function names snake_case; prefer values/immutability/RAII; `std::move` only when consuming; `std::forward` only for forwarding refs; rely on copy elision; feature‑test‑gate C++26; compile warning‑free; optimize last.
