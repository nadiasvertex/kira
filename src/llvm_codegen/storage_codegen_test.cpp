// LLVM compiled from the shared storage form. The value cases are the ones the
// bytecode compiler passes (`src/testing/storage_cases.h`), run through the JIT
// at two optimization levels; the structural checks look at the IR itself.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wshadow"
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#pragma clang diagnostic pop

#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/llvm_codegen/codegen.h"
#include "src/llvm_codegen/jit_support.h"
#include "src/llvm_codegen/storage_codegen.h"
#include "src/testing/storage_cases.h"
#include "src/testing/storage_intrinsic_cases.h"
#include "src/testing/storage_closure_cases.h"
#include "src/testing/storage_pattern_cases.h"
#include "src/testing/storage_memory_cases.h"
#include "src/testing/storage_fixture.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::testing::expect;
using cinder::testing::fail;
namespace bytecode = cinder::bytecode;
namespace cases = cinder::testing::storage_cases;
namespace llvm_codegen = cinder::llvm_codegen;
using cases::expect_equal;

/// The fixture's storage form compiled to LLVM, with entry thunks.
[[nodiscard]] auto compile(cinder::testing::storage_program &program)
    -> llvm_codegen::compiled_module {
  auto compiled = llvm_codegen::compile_storage_module(
      program.lowering.module, *program.env,
      llvm_codegen::storage_codegen_options{.entry_thunks = true});
  if (!compiled.has_value()) {
    std::cerr << compiled.error().message << "\n";
    fail("expected the storage form to compile to LLVM");
  }
  return std::move(*compiled);
}

/// A fixture compiled to LLVM and loaded into the JIT.
class llvm_program final : public cases::case_runner {
public:
  llvm_program(std::unique_ptr<cinder::testing::storage_program> program,
               llvm_codegen::optimization_level level)
      : program_(std::move(program)) {
    auto created = llvm_codegen::jit_module::create(compile(*program_), level);
    if (!created.has_value()) {
      std::cerr << created.error() << "\n";
      fail("expected the module to load into the JIT");
    }
    jit_ = std::make_unique<llvm_codegen::jit_module>(std::move(*created));
  }

  [[nodiscard]] auto scalar(std::string_view name, std::span<const int64_t> args)
      -> int64_t override {
    auto raw = std::array<std::byte, 8>{};
    const auto ran = jit_->run_storage(name, slots(args), raw.data());
    if (!ran.has_value()) {
      std::cerr << name << " panicked: "
                << bytecode::panic_reason_message(ran.error()) << "\n";
      fail("expected the function to return");
    }
    return decode(name, raw);
  }

  [[nodiscard]] auto panics(std::string_view name, std::span<const int64_t> args)
      -> std::optional<bytecode::panic_reason> override {
    auto raw = std::array<std::byte, 8>{};
    const auto ran = jit_->run_storage(name, slots(args), raw.data());
    if (ran.has_value()) {
      return std::nullopt;
    }
    return ran.error();
  }

  [[nodiscard]] auto into(std::string_view name, std::span<const int64_t> args)
      -> cases::result_bytes override {
    auto out = cases::result_bytes{};
    const auto ran = jit_->run_storage(name, slots(args), out.bytes.data());
    if (!ran.has_value()) {
      std::cerr << name << " panicked: "
                << bytecode::panic_reason_message(ran.error()) << "\n";
      fail("expected the function to return");
    }
    return out;
  }

  [[nodiscard]] auto scalar_at(std::string_view name, void *argument)
      -> int64_t override {
    auto raw = std::array<std::byte, 8>{};
    const auto address = std::array<uint64_t, 1>{
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(argument))};
    const auto ran = jit_->run_storage(name, address, raw.data());
    expect(ran.has_value(), "expected the function to return");
    return decode(name, raw);
  }

private:
  [[nodiscard]] static auto slots(std::span<const int64_t> args)
      -> std::vector<uint64_t> {
    auto out = std::vector<uint64_t>{};
    for (const auto arg : args) {
      out.push_back(static_cast<uint64_t>(arg));
    }
    return out;
  }

  /// A scalar result is written at its own width: extend it by its type.
  [[nodiscard]] auto decode(std::string_view name,
                            const std::array<std::byte, 8> &raw) const
      -> int64_t {
    const auto *fn = program_->lowering.module.find(name);
    expect(fn != nullptr, "expected the function to be compiled");
    const auto &types = program_->env->types();
    const auto kind = bytecode::numeric_kind_of(
        types, types.strip_refinement(fn->signature.result.type));
    const auto is_signed = kind.has_value() && bytecode::is_signed_integer(*kind);
    switch (fn->signature.result.size) {
    case 1:
      return is_signed ? read<int8_t>(raw) : read<uint8_t>(raw);
    case 2:
      return is_signed ? read<int16_t>(raw) : read<uint16_t>(raw);
    case 4:
      return is_signed ? read<int32_t>(raw) : read<uint32_t>(raw);
    default:
      return read<int64_t>(raw);
    }
  }

  template <typename T>
  [[nodiscard]] static auto read(const std::array<std::byte, 8> &raw)
      -> int64_t {
    auto value = T{};
    std::memcpy(&value, raw.data(), sizeof(T));
    return static_cast<int64_t>(value);
  }

  std::unique_ptr<cinder::testing::storage_program> program_;
  std::unique_ptr<llvm_codegen::jit_module> jit_;
};

class llvm_backend final : public cases::case_backend {
public:
  explicit llvm_backend(llvm_codegen::optimization_level level)
      : level_(level) {}

  [[nodiscard]] auto open(std::string_view dir, std::string_view name)
      -> std::unique_ptr<cases::case_runner> override {
    return std::make_unique<llvm_program>(
        cinder::testing::build_storage_program(
            cinder::testing::read_program(dir, name)),
        level_);
  }

private:
  llvm_codegen::optimization_level level_;
};

// ==========================================================================
//  The IR itself
// ==========================================================================

/// The pointer an access goes through, followed back past its byte offsets.
[[nodiscard]] auto base_of(const llvm::Value *pointer) -> const llvm::Value * {
  while (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer)) {
    pointer = gep->getPointerOperand();
  }
  return pointer;
}

auto test_signatures_follow_the_internal_abi() -> void {
  auto program = cinder::testing::build_storage_program(
      cinder::testing::read_program("bytecode_storage_test", "values.cn"));
  auto compiled = compile(*program);
  const auto &module = *compiled.module;

  const auto *line = module.getFunction("make_line");
  expect(line != nullptr, "expected make_line");
  // The hidden result address comes first, then the source parameter; the
  // result itself is not returned.
  expect(line->arg_size() == 2 && line->getArg(0)->getType()->isPointerTy() &&
             line->getArg(1)->getType()->isIntegerTy(32) &&
             line->getReturnType()->isVoidTy(),
         "expected make_line(ptr, i32) -> void");

  const auto *take = module.getFunction("take_pair");
  expect(take != nullptr && take->arg_size() == 2 &&
             take->getArg(0)->getType()->isPointerTy() &&
             take->getArg(1)->getType()->isIntegerTy(32) &&
             take->getReturnType()->isIntegerTy(32),
         "expected take_pair(ptr, i32) -> i32: the pair travels by address");

  const auto *wide = module.getFunction("make_wide");
  expect(wide != nullptr && wide->arg_size() == 2 &&
             wide->getArg(1)->getType()->isIntegerTy(64) &&
             wide->getReturnType()->isVoidTy(),
         "expected make_wide(ptr, i64) -> void");

  const auto *bind = module.getFunction("bind");
  expect(bind != nullptr && bind->arg_size() == 1 &&
             bind->getReturnType()->isIntegerTy(32),
         "expected a scalar function to keep its scalar signature");

  // A sub-word result carries its extension, as a native caller expects.
  const auto *small = module.getFunction("neg_literal");
  expect(small != nullptr && small->getReturnType()->isIntegerTy(8) &&
             small->hasRetAttribute(llvm::Attribute::SExt),
         "expected neg_literal() -> signext i8");

  // Entry thunks exist for every function.
  expect(module.getFunction("__cinder_entry_make_line") != nullptr,
         "expected an entry thunk");
}

auto test_no_allocation_symbol_is_declared_or_called() -> void {
  auto program = cinder::testing::build_storage_program(
      cinder::testing::read_program("bytecode_storage_test", "values.cn"));
  auto compiled = compile(*program);
  expect(compiled.module->getFunction(llvm_codegen::kAllocSymbolName) == nullptr,
         "expected no heap allocation entry point in the module");
  for (const auto &function : *compiled.module) {
    for (const auto &block : function) {
      for (const auto &instruction : block) {
        const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call == nullptr || call->getCalledFunction() == nullptr) {
          continue;
        }
        expect(call->getCalledFunction()->getName() !=
                   llvm_codegen::kAllocSymbolName,
               "expected no call to the heap allocator");
      }
    }
  }
}

auto test_entry_thunks_are_optional() -> void {
  auto program = cinder::testing::build_storage_program(
      cinder::testing::read_program("bytecode_storage_test", "values.cn"));
  auto compiled = llvm_codegen::compile_storage_module(program->lowering.module,
                                                       *program->env);
  expect(compiled.has_value(), "expected the module to compile");
  expect(compiled->module->getFunction("__cinder_entry_make_line") == nullptr,
         "expected no entry thunks unless asked for");
}

/// A packed struct has alignment 1, so nothing read or written through an
/// address that reaches it — a parameter, the result, or a loaded reference —
/// may claim more, whatever the type of the field.
auto test_accesses_through_addresses_claim_only_the_alignment_they_have()
    -> void {
  auto program = cinder::testing::build_storage_program(
      cinder::testing::read_program("bytecode_storage_test", "packed.cn"));
  auto compiled = compile(*program);
  auto checked_accesses = size_t{0};
  for (const auto name :
       {"make_header", "make_framed", "read_len", "set_len"}) {
    const auto *function = compiled.module->getFunction(name);
    expect(function != nullptr, "expected the function");
    for (const auto &block : *function) {
      for (const auto &instruction : block) {
        const llvm::Value *pointer = nullptr;
        const llvm::Type *accessed = nullptr;
        auto align = llvm::Align{};
        if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
          pointer = load->getPointerOperand();
          accessed = load->getType();
          align = load->getAlign();
        } else if (const auto *store =
                       llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
          pointer = store->getPointerOperand();
          accessed = store->getValueOperand()->getType();
          align = store->getAlign();
        } else {
          continue;
        }
        const auto *base = base_of(pointer);
        const auto through_address =
            llvm::isa<llvm::Argument>(base) || llvm::isa<llvm::LoadInst>(base);
        if (!through_address || !accessed->isIntegerTy() ||
            accessed->getIntegerBitWidth() <= 8) {
          continue;
        }
        ++checked_accesses;
        if (align.value() != 1) {
          std::cerr << name << ": a " << accessed->getIntegerBitWidth()
                    << "-bit access claims alignment " << align.value() << "\n";
          fail("expected an access to packed bytes to claim alignment 1");
        }
      }
    }
  }
  // The scan must have found the accesses it is about.
  expect(checked_accesses >= 4, "expected to check the packed field accesses");
}

auto test_every_case_passes_at_each_optimization_level() -> void {
  for (const auto level : {llvm_codegen::optimization_level::o0,
                           llvm_codegen::optimization_level::o2}) {
    auto backend = llvm_backend{level};
    cases::run_all(backend);
    cases::intrinsic_cases(backend);
    cases::memory_cases(backend);
    cases::closure_cases(backend);
    cases::pattern_cases(backend);
  }
}

} // namespace

auto main() -> int {
  test_signatures_follow_the_internal_abi();
  test_no_allocation_symbol_is_declared_or_called();
  test_entry_thunks_are_optional();
  test_accesses_through_addresses_claim_only_the_alignment_they_have();
  test_every_case_passes_at_each_optimization_level();
  std::cout << "storage_codegen_test: ok\n";
  return 0;
}
