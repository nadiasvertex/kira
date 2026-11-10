#pragma once

// Cases for calls to runtime intrinsics from the shared storage form, run by
// every backend that compiles it. The expected bytes are worked out by hand
// from the layouts in `src/runtime/result_out.h` (a `result` is `{tag: u64,
// payload @8}`, a `str` is `{len, data}`), and the host effects are checked
// against the operating system, not against another backend.

#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "src/runtime/allocator.h"
#include "src/testing/storage_cases.h"
#include "src/testing/test_assert.h"

namespace cinder::testing::storage_cases {

/// A `str` or `slice[byte]` descriptor the host owns: the layout the compiler
/// gives them, `{len: u64 @0, data: address @8}`.
struct text_view {
  uint64_t length = 0;
  const char *data = nullptr;

  explicit text_view(std::string_view text)
      : length(text.size()), data(text.data()) {}
};

static_assert(sizeof(text_view) == 16);

/// A descriptor passed to a function as the address of a parameter.
[[nodiscard]] inline auto as_argument(const text_view &view) -> int64_t {
  return static_cast<int64_t>(reinterpret_cast<intptr_t>(&view)); // NOLINT
}

/// The text a `str` result holds, released with the allocator that made it.
[[nodiscard]] inline auto take_text(const result_bytes &result,
                                    size_t offset = 0) -> std::string {
  const auto length = result.at<uint64_t>(offset);
  const auto *data = reinterpret_cast<const char *>( // NOLINT
      result.at<uint64_t>(offset + 8));
  auto text = std::string(data == nullptr ? "" : data, length);
  cinder_heap_free(const_cast<char *>(data), length); // NOLINT
  return text;
}

inline auto expect_text(const result_bytes &result, std::string_view wanted,
                        std::string_view what) -> void {
  const auto actual = take_text(result);
  if (actual != wanted) {
    std::cerr << what << ": expected \"" << wanted << "\", got \"" << actual
              << "\"\n";
    fail("a string result differs");
  }
}

inline auto expect_ok(const result_bytes &result, std::string_view what)
    -> void {
  expect_equal(static_cast<int64_t>(result.at<uint64_t>(0)), 0,
               std::string(what) + " is ok");
}

inline auto expect_err(const result_bytes &result, int code,
                       std::string_view what) -> void {
  expect_equal(static_cast<int64_t>(result.at<uint64_t>(0)), 1,
               std::string(what) + " is err");
  expect_equal(result.at<int32_t>(8), code,
               std::string(what) + " error code");
}

/// A descriptor argument that has to stay alive for the call.
[[nodiscard]] inline auto bits_of(double value) -> int64_t {
  return std::bit_cast<int64_t>(value);
}

// ==========================================================================
//  I/O results
// ==========================================================================

inline auto io_results_are_written_in_place(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "intrinsics.cn");

  // A `raw_fd` returned as it arrives, and read through a field.
  expect_equal(p->into("stdout_value", {}).at<int64_t>(0), 1, "stdout value");
  expect_equal(p->scalar("stdout_number", {}), 1, "stdout field");

  // A failing call: `err(io_errno)`, the code inline at offset 8.
  auto bad = p->into("close_fd", std::vector<int64_t>{1000000});
  expect_err(bad, EBADF, "close of a descriptor that is not open");

  // Success and then failure of the same call on a real descriptor. The whole
  // `ok` result is the tag and zero bytes: nothing else is left unset.
  auto ends = std::vector<int>(2, -1);
  expect(::pipe(ends.data()) == 0, "expected the host to make a pipe");
  auto closed = p->into("close_fd", std::vector<int64_t>{ends[0]});
  expect_ok(closed, "close of an open descriptor");
  expect_equal(static_cast<int64_t>(closed.at<uint64_t>(8)), 0,
               "bytes after the ok tag");
  expect_err(p->into("close_fd", std::vector<int64_t>{ends[0]}), EBADF,
             "second close");

  // The result is dropped: the effect still happens.
  expect_equal(p->scalar("discard_close", std::vector<int64_t>{ends[1]}), 7,
               "discard_close");
  expect(::fcntl(ends[1], F_GETFD) == -1 && errno == EBADF,
         "expected the discarded call to have closed the descriptor");

  expect_ok(p->into("flush_fd", std::vector<int64_t>{1}), "flush");

  // A runtime-made pipe, written and read through the same functions.
  auto made = p->into("make_pipe", {});
  expect_ok(made, "pipe");
  const auto read_end = made.at<int64_t>(8);
  const auto write_end = made.at<int64_t>(16);
  expect(read_end >= 0 && write_end >= 0 && read_end != write_end,
         "expected two distinct descriptors");

  const auto message = std::string_view("ping");
  const auto outgoing = text_view(message);
  auto sent = p->into("write_bytes",
                      std::vector<int64_t>{write_end, as_argument(outgoing)});
  expect_ok(sent, "write");
  expect_equal(sent.at<int64_t>(8), 4, "bytes written");

  auto buffer = std::string(16, '\0');
  const auto incoming = text_view(buffer);
  auto received = p->into("read_bytes", std::vector<int64_t>{
                                            read_end, as_argument(incoming)});
  expect_ok(received, "read");
  expect_equal(received.at<int64_t>(8), 4, "bytes read");
  expect(buffer.substr(0, 4) == "ping", "expected the bytes that were sent");

  // Reading an empty non-blocking descriptor reports the host's EAGAIN.
  expect_ok(p->into("nonblocking", std::vector<int64_t>{read_end}),
            "set_nonblocking");
  auto empty = p->into("read_bytes", std::vector<int64_t>{
                                          read_end, as_argument(incoming)});
  expect_err(empty, static_cast<int>(p->scalar("would_block", {})),
             "read of an empty pipe");
  expect_equal(p->scalar("would_block", {}), EAGAIN, "would_block");

  expect_ok(p->into("close_fd", std::vector<int64_t>{read_end}), "close read");
  expect_err(p->into("write_bytes", std::vector<int64_t>{
                                        read_end, as_argument(outgoing)}),
             EBADF, "write to a closed descriptor");
  expect_ok(p->into("close_fd", std::vector<int64_t>{write_end}),
            "close write");
}

/// Repeating success and failure paths neither allocates a result wrapper nor
/// leaves a block behind.
inline auto repeated_io_allocates_nothing(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "intrinsics.cn");
  const auto allocations = runtime::heap_allocation_count();
  const auto live = runtime::heap_live_block_count();
  for (int round = 0; round < 500; ++round) {
    auto made = p->into("make_pipe", {});
    expect_ok(made, "pipe");
    const auto read_end = made.at<int64_t>(8);
    const auto write_end = made.at<int64_t>(16);
    expect_ok(p->into("close_fd", std::vector<int64_t>{read_end}), "close");
    expect_equal(p->scalar("discard_close", std::vector<int64_t>{write_end}), 7,
                 "discard_close");
    expect_err(p->into("close_fd", std::vector<int64_t>{write_end}), EBADF,
               "close after discard");
  }
  expect_equal(static_cast<int64_t>(runtime::heap_allocation_count() -
                                    allocations),
               0, "heap blocks allocated by repeated pipe calls");
  expect_equal(runtime::heap_live_block_count() - live, 0,
               "heap blocks left by repeated pipe calls");
}

// ==========================================================================
//  String results
// ==========================================================================

inline auto string_results_are_written_in_place(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "intrinsics.cn");
  const auto text = [](std::string_view s) { return text_view(s); };

  {
    const auto s = text("abc");
    expect_text(p->into("reverse", std::vector<int64_t>{as_argument(s)}), "cba",
                "reverse");
    const auto multi = text("a\xC3\xB1" "b"); // "añb"
    expect_text(p->into("reverse", std::vector<int64_t>{as_argument(multi)}),
                "b\xC3\xB1" "a", "reverse of a multibyte string");
    expect_equal(p->scalar("length", std::vector<int64_t>{as_argument(multi)}),
                 3, "length counts scalars");
    expect_text(
        p->into("truncate",
                std::vector<int64_t>{as_argument(multi), int64_t{2}}),
        "a\xC3\xB1", "truncate");
  }
  {
    const auto a = text("ab");
    const auto b = text("cde");
    expect_text(p->into("concat", std::vector<int64_t>{as_argument(a),
                                                       as_argument(b)}),
                "abcde", "concat");
    expect_equal(p->scalar("same", std::vector<int64_t>{as_argument(a),
                                                        as_argument(a)}),
                 1, "same");
    expect_equal(p->scalar("same", std::vector<int64_t>{as_argument(a),
                                                        as_argument(b)}),
                 0, "different");
    expect(p->scalar("compare", std::vector<int64_t>{as_argument(a),
                                                     as_argument(b)}) < 0,
           "expected \"ab\" to sort before \"cde\"");
    expect(p->scalar("compare", std::vector<int64_t>{as_argument(b),
                                                     as_argument(a)}) > 0,
           "expected \"cde\" to sort after \"ab\"");
  }
  {
    const auto haystack = text("hello world");
    const auto needle = text("o");
    const auto missing = text("z");
    auto first = p->into("find", std::vector<int64_t>{as_argument(haystack),
                                                      as_argument(needle), 0});
    expect_equal(first.at<uint8_t>(0), 1, "find.found");
    expect_equal(first.at<int64_t>(8), 4, "find.pos");
    auto second = p->into("find", std::vector<int64_t>{as_argument(haystack),
                                                       as_argument(needle), 5});
    expect_equal(second.at<int64_t>(8), 7, "find from 5");
    auto none = p->into("find", std::vector<int64_t>{as_argument(haystack),
                                                     as_argument(missing), 0});
    expect_equal(none.at<uint8_t>(0), 0, "find of a missing string");
    expect_equal(none.at<int64_t>(8), 0, "find.pos when not found");
    auto last = p->into("rfind", std::vector<int64_t>{as_argument(haystack),
                                                      as_argument(needle)});
    expect_equal(last.at<uint8_t>(0), 1, "rfind.found");
    expect_equal(last.at<int64_t>(8), 7, "rfind.pos");
  }
  {
    const auto padded = text("  hi  ");
    expect_text(p->into("trim", std::vector<int64_t>{as_argument(padded), 0}),
                "hi", "trim both");
    expect_text(p->into("trim", std::vector<int64_t>{as_argument(padded), 1}),
                "hi  ", "trim start");
    expect_text(p->into("trim", std::vector<int64_t>{as_argument(padded), 2}),
                "  hi", "trim end");
    const auto source = text("a-b-c");
    const auto dash = text("-");
    const auto plus = text("+");
    expect_text(p->into("replace", std::vector<int64_t>{as_argument(source),
                                                        as_argument(dash),
                                                        as_argument(plus)}),
                "a+b+c", "replace");
  }
  expect_text(p->into("repeat", std::vector<int64_t>{'x', 3}), "xxx", "repeat");
  expect_text(p->into("codepoint", std::vector<int64_t>{65}), "A", "codepoint");
  expect_text(p->into("codepoint", std::vector<int64_t>{0x1F600}),
              "\xF0\x9F\x98\x80", "a four-byte codepoint");
  expect_text(p->into("radix", std::vector<int64_t>{255, 16, 0}), "ff", "hex");
  expect_text(p->into("radix", std::vector<int64_t>{255, 16, 1}), "FF",
              "uppercase hex");
  expect_text(p->into("radix", std::vector<int64_t>{5, 2, 0}), "101", "binary");
  expect_text(p->into("radix", std::vector<int64_t>{0, 10, 0}), "0", "zero");
  expect_text(p->into("fixed", std::vector<int64_t>{bits_of(3.14159), 2}),
              "3.14", "fixed");
  expect_text(
      p->into("scientific", std::vector<int64_t>{bits_of(1234.5), 2, 0}),
      "1.23e3", "scientific");
  expect_text(
      p->into("scientific", std::vector<int64_t>{bits_of(1234.5), 2, 1}),
      "1.23E3", "uppercase scientific");
  expect_text(p->into("general", std::vector<int64_t>{bits_of(0.5), 6}), "0.5",
              "general");
}

/// A `str` result costs one allocation, for its bytes, and none for the
/// descriptor; an empty one costs nothing.
inline auto string_results_allocate_only_their_bytes(case_backend &backend)
    -> void {
  auto p = backend.open("bytecode_storage_test", "intrinsics.cn");
  const auto source = text_view("hello");
  const auto empty = text_view("");

  auto before = runtime::heap_allocation_count();
  auto reversed = p->into("reverse", std::vector<int64_t>{as_argument(source)});
  expect_equal(static_cast<int64_t>(runtime::heap_allocation_count() - before),
               1, "allocations for one string result");
  expect_text(reversed, "olleh", "reverse");

  before = runtime::heap_allocation_count();
  auto nothing = p->into("concat", std::vector<int64_t>{as_argument(empty),
                                                        as_argument(empty)});
  expect_equal(static_cast<int64_t>(runtime::heap_allocation_count() - before),
               0, "allocations for an empty string result");
  expect_equal(static_cast<int64_t>(nothing.at<uint64_t>(0)), 0,
               "empty string length");
  expect_equal(static_cast<int64_t>(nothing.at<uint64_t>(8)), 0,
               "empty string data");

  const auto live = runtime::heap_live_block_count();
  for (int round = 0; round < 500; ++round) {
    expect_text(p->into("reverse", std::vector<int64_t>{as_argument(source)}),
                "olleh", "reverse in a loop");
  }
  expect_equal(runtime::heap_live_block_count() - live, 0,
               "string blocks left behind");
}

inline auto intrinsic_cases(case_backend &backend) -> void {
  io_results_are_written_in_place(backend);
  repeated_io_allocates_nothing(backend);
  string_results_are_written_in_place(backend);
  string_results_allocate_only_their_bytes(backend);
}

} // namespace cinder::testing::storage_cases
