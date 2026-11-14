// Calls the `_into` runtime entry points directly, as both backends do, and
// checks the bytes they write against the operating system and hand-computed
// values. A `result` is `{tag: u64 @0, payload @8}`; a `str` is `{len, data}`.

#include <sys/utsname.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include "src/runtime/allocator.h"
#include "src/runtime/fmt.h"
#include "src/runtime/io.h"
#include "src/runtime/platform.h"
#include "src/runtime/result_out.h"
#include "src/runtime/string.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::testing::expect;

/// Bytes filled with a pattern that is not a valid value, so a byte the
/// runtime does not write is visible.
struct buffer {
  alignas(16) std::array<std::byte, 128> bytes{};
  buffer() { bytes.fill(std::byte{0xAA}); }
  [[nodiscard]] auto data() -> void * { return bytes.data(); }
  template <typename T> [[nodiscard]] auto at(size_t offset) const -> T {
    auto value = T{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
  }
};

struct text {
  uint64_t length;
  const char *data;
};

[[nodiscard]] auto view(std::string_view s) -> text {
  return text{s.size(), s.data()};
}

/// The string at `offset`, freed.
[[nodiscard]] auto take(const buffer &b, size_t offset) -> std::string {
  const auto length = b.at<uint64_t>(offset);
  auto *data = reinterpret_cast<char *>(b.at<uint64_t>(offset + 8)); // NOLINT
  auto s = std::string(data == nullptr ? "" : data, length);
  if (data != nullptr) {
    cinder_heap_free(data, length);
  }
  return s;
}

struct raw_fd {
  int64_t value;
};

auto test_pipe_round_trip_and_errors() -> void {
  const auto live = cinder::runtime::heap_live_block_count();
  const auto allocations = cinder::runtime::heap_allocation_count();
  for (int round = 0; round < 200; ++round) {
    auto made = buffer{};
    cinder_rt_pipe_into(made.data());
    expect(made.at<uint64_t>(0) == 0, "expected pipe to succeed");
    const auto read_end = raw_fd{made.at<int64_t>(8)};
    const auto write_end = raw_fd{made.at<int64_t>(16)};

    const auto message = std::string_view("hello");
    const auto outgoing = view(message);
    auto sent = buffer{};
    cinder_rt_write_into(sent.data(), &write_end, &outgoing);
    expect(sent.at<uint64_t>(0) == 0 && sent.at<int64_t>(8) == 5,
           "expected five bytes to be written");

    auto storage = std::array<char, 16>{};
    const auto incoming = text{storage.size(), storage.data()};
    auto received = buffer{};
    cinder_rt_read_into(received.data(), &read_end, &incoming);
    expect(received.at<uint64_t>(0) == 0 && received.at<int64_t>(8) == 5,
           "expected five bytes to be read");
    expect(std::string_view(storage.data(), 5) == "hello",
           "expected the bytes that were written");

    auto closed = buffer{};
    cinder_rt_close_into(closed.data(), &read_end);
    expect(closed.at<uint64_t>(0) == 0, "expected close to succeed");
    expect(closed.at<uint64_t>(8) == 0,
           "expected every byte of a unit result to be written");
    expect(closed.at<uint64_t>(16) == 0xAAAAAAAAAAAAAAAAULL,
           "expected the write to stop at the result's size");
    cinder_rt_close_into(closed.data(), &read_end);
    expect(closed.at<uint64_t>(0) == 1 && closed.at<int32_t>(8) == EBADF,
           "expected the second close to report EBADF");
    cinder_rt_close_into(closed.data(), &write_end);
    expect(closed.at<uint64_t>(0) == 0, "expected close to succeed");
  }
  expect(cinder::runtime::heap_allocation_count() == allocations,
         "expected the io functions to allocate nothing");
  expect(cinder::runtime::heap_live_block_count() == live,
         "expected the io functions to leave no block behind");
}

auto test_open_reports_the_host_error() -> void {
  const auto path = view("/definitely/not/a/real/path");
  const auto options = std::array<uint8_t, 5>{1, 0, 0, 0, 0};
  auto result = buffer{};
  cinder_rt_open_into(result.data(), &path, options.data());
  expect(result.at<uint64_t>(0) == 1 && result.at<int32_t>(8) == ENOENT,
         "expected ENOENT for a missing file");

  const auto here = view("/dev/null");
  cinder_rt_open_into(result.data(), &here, options.data());
  expect(result.at<uint64_t>(0) == 0, "expected /dev/null to open");
  const auto fd = raw_fd{result.at<int64_t>(8)};
  expect(fd.value >= 0, "expected a descriptor");
  cinder_rt_close_into(result.data(), &fd);
  expect(result.at<uint64_t>(0) == 0, "expected close to succeed");
}

auto test_platform_queries_match_the_host() -> void {
  auto host = std::array<char, 256>{};
  expect(::gethostname(host.data(), host.size() - 1) == 0, "gethostname");
  auto name = buffer{};
  cinder_rt_gethostname_into(name.data());
  expect(name.at<uint64_t>(0) == 0, "expected the hostname query to succeed");
  expect(take(name, 8) == std::string(host.data()),
         "expected the hostname the host reports");

  auto native = utsname{};
  expect(::uname(&native) == 0, "uname");
  auto info = buffer{};
  cinder_rt_uname_into(info.data());
  expect(info.at<uint64_t>(0) == 0, "expected the uname query to succeed");
  expect(take(info, 8) == native.sysname, "uname sysname");
  expect(take(info, 24) == native.nodename, "uname nodename");
  expect(take(info, 40) == native.release, "uname release");
  expect(take(info, 56) == native.version, "uname version");
  expect(take(info, 72) == native.machine, "uname machine");
}

auto test_strings_are_written_in_place() -> void {
  auto result = buffer{};
  const auto abc = view("abc");
  cinder_rt_str_reverse_into(result.data(), &abc);
  expect(take(result, 0) == "cba", "reverse");

  const auto hello = view("hello world");
  const auto needle = view("o");
  cinder_rt_str_find_into(result.data(), &hello, &needle, 5);
  expect(result.at<uint8_t>(0) == 1 && result.at<uint64_t>(8) == 7,
         "find from 5");
  const auto missing = view("z");
  cinder_rt_str_find_into(result.data(), &hello, &missing, 0);
  expect(result.at<uint8_t>(0) == 0 && result.at<uint64_t>(8) == 0,
         "a find that fails leaves no position");

  cinder_rt_fmt_radix_digits_into(result.data(), 255, 16, 1);
  expect(take(result, 0) == "FF", "radix");
  cinder_rt_fmt_char_from_codepoint_into(result.data(), 0x1F600);
  expect(take(result, 0) == "\xF0\x9F\x98\x80", "codepoint");

  const auto none = view("");
  const auto before = cinder::runtime::heap_allocation_count();
  cinder_rt_str_concat_into(result.data(), &none, &none);
  expect(cinder::runtime::heap_allocation_count() == before,
         "expected an empty string to cost no allocation");
  expect(result.at<uint64_t>(0) == 0 && result.at<uint64_t>(8) == 0,
         "expected an empty descriptor");
}

} // namespace

auto main() -> int {
  test_pipe_round_trip_and_errors();
  test_open_reports_the_host_error();
  test_platform_queries_match_the_host();
  test_strings_are_written_in_place();
  return 0;
}
