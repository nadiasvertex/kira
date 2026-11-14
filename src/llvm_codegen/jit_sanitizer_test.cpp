// JIT-compiled code is instrumented for the sanitizer the host was built
// with (`instrument_for_host_sanitizer`), and still runs: under
// `--config=asan` the module calls into AddressSanitizer, under
// `--config=tsan` into ThreadSanitizer, and in a plain build into neither.
#include <format>
#include <memory>
#include <string>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wshadow"
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#pragma clang diagnostic pop

#include "src/llvm_codegen/jit_support.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::testing::expect;
namespace lc = cinder::llvm_codegen;

/// A stack slot, a global, and a load and store of each: the accesses an
/// instrumented function checks.
constexpr auto k_program = R"(
@g = global i64 41

define i64 @main() {
  %p = alloca i64
  store i64 1, ptr %p
  %a = load i64, ptr %p
  %b = load i64, ptr @g
  %c = add i64 %a, %b
  ret i64 %c
}
)";

auto parse() -> lc::compiled_module {
  auto context = std::make_unique<llvm::LLVMContext>();
  auto error = llvm::SMDiagnostic{};
  auto module = llvm::parseAssemblyString(k_program, error, *context);
  expect(module != nullptr, "expected the test IR to parse");
  return lc::compiled_module{.context = std::move(context),
                             .module = std::move(module)};
}

auto text_of(const llvm::Module &module) -> std::string {
  auto out = std::string{};
  auto stream = llvm::raw_string_ostream(out);
  module.print(stream, nullptr);
  return out;
}

auto test_instruments_for_the_host_sanitizer() -> void {
  auto compiled = parse();
  lc::instrument_for_host_sanitizer(*compiled.module);
  const auto ir = text_of(*compiled.module);
  const auto sanitizer = lc::current_host_sanitizer();
  const auto asan = ir.contains("__asan_");
  const auto tsan = ir.contains("__tsan_");
  expect(asan == (sanitizer == lc::host_sanitizer::address),
         std::format("expected AddressSanitizer calls exactly when the host "
                     "is built with it:\n{}",
                     ir));
  expect(tsan == (sanitizer == lc::host_sanitizer::thread),
         std::format("expected ThreadSanitizer calls exactly when the host "
                     "is built with it:\n{}",
                     ir));
}

auto test_instrumented_code_runs() -> void {
  auto jit = lc::jit_module::create(parse());
  expect(jit.has_value(), "expected the instrumented module to load: " +
                              (jit.has_value() ? std::string{} : jit.error()));
  const auto ran = jit->run("main", cinder::bytecode::numeric_kind::i64);
  expect(ran.has_value() && ran->has_value && ran->value.i == 42,
         "expected the instrumented `main` to return 42");
}

} // namespace

auto main() -> int {
  test_instruments_for_the_host_sanitizer();
  test_instrumented_code_runs();
  return 0;
}
