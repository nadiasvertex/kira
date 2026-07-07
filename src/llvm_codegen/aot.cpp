#include "src/llvm_codegen/aot.h"

#include <format>
#include <memory>
#include <system_error>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

namespace kira::llvm_codegen {

namespace {

/// Adds a fresh `i32 @main()` to `module` that calls `entry_fn` (already
/// verified zero-argument by the caller) and returns a process exit code
/// derived from its result. `entry_fn` is renamed first (to whatever its
/// declared name plus the same convention `emit_object_file` documents) so
/// this doesn't collide with it when the source module's chosen entry
/// function happens to already be named `main`.
[[nodiscard]] auto synthesize_c_main(llvm::LLVMContext &ctx,
                                     llvm::Module &module,
                                     llvm::Function &entry_fn)
    -> std::expected<void, aot_error> {
  auto *return_ty = entry_fn.getReturnType();
  if (return_ty->isFloatingPointTy()) {
    return std::unexpected(aot_error{
        .message = "the entry function must return an integer, `bool`, or "
                   "`unit` type to be used as `kira build`'s process exit "
                   "code — a floating-point return value is not a sensible "
                   "exit status"});
  }
  if (!return_ty->isVoidTy() && !return_ty->isIntegerTy()) {
    return std::unexpected(aot_error{
        .message = "the entry function's return type has no process exit "
                   "code mapping"});
  }

  entry_fn.setName("__kira_entry");

  auto *i32_ty = llvm::Type::getInt32Ty(ctx);
  auto *main_ty = llvm::FunctionType::get(i32_ty, /*isVarArg=*/false);
  auto *main_fn = llvm::Function::Create(
      main_ty, llvm::Function::ExternalLinkage, "main", module);
  auto *entry_bb = llvm::BasicBlock::Create(ctx, "entry", main_fn);
  auto builder = llvm::IRBuilder<>(entry_bb);
  auto *call = builder.CreateCall(&entry_fn, {});

  if (return_ty->isVoidTy()) {
    builder.CreateRet(llvm::ConstantInt::get(i32_ty, 0));
  } else if (return_ty->isIntegerTy(1)) {
    builder.CreateRet(builder.CreateZExt(call, i32_ty));
  } else {
    builder.CreateRet(builder.CreateSExtOrTrunc(call, i32_ty));
  }

  return {};
}

} // namespace

auto emit_object_file(compiled_module module,
                      std::string_view entry_function_name,
                      const std::string &object_path)
    -> std::expected<void, aot_error> {
  auto &ctx = *module.context;
  auto &llvm_module = *module.module;

  auto *entry_fn = llvm_module.getFunction(std::string(entry_function_name));
  if (entry_fn == nullptr) {
    return std::unexpected(aot_error{
        .message =
            std::format("module `{}` has no function named `{}`",
                        llvm_module.getName().str(), entry_function_name)});
  }
  if (entry_fn->arg_size() != 0) {
    return std::unexpected(aot_error{
        .message = std::format("cannot build `{}`: `kira build` only "
                               "supports zero-parameter entry functions",
                               entry_function_name)});
  }

  if (auto result = synthesize_c_main(ctx, llvm_module, *entry_fn);
      !result.has_value()) {
    return std::unexpected(result.error());
  }

  auto verify_message = std::string{};
  auto verify_stream = llvm::raw_string_ostream(verify_message);
  if (llvm::verifyModule(llvm_module, &verify_stream)) {
    return std::unexpected(aot_error{
        .message = std::format("llvm_codegen produced a module that failed "
                               "verification after adding the AOT entry "
                               "point (this is a codegen bug, not a source "
                               "error): {}",
                               verify_message)});
  }

  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();

  const auto triple_str = llvm::sys::getDefaultTargetTriple();
  const auto triple = llvm::Triple(triple_str);
  auto lookup_error = std::string{};
  const auto *target = llvm::TargetRegistry::lookupTarget(triple, lookup_error);
  if (target == nullptr) {
    return std::unexpected(aot_error{
        .message = std::format("could not find an LLVM target for `{}`: {}",
                               triple_str, lookup_error)});
  }
  auto options = llvm::TargetOptions{};
  std::unique_ptr<llvm::TargetMachine> target_machine(
      target->createTargetMachine(triple, llvm::sys::getHostCPUName(), "",
                                  options, llvm::Reloc::PIC_));
  if (!target_machine) {
    return std::unexpected(aot_error{
        .message = std::format("could not create a target machine for `{}`",
                               triple_str)});
  }

  llvm_module.setDataLayout(target_machine->createDataLayout());
  llvm_module.setTargetTriple(triple);

  auto ec = std::error_code{};
  auto dest = llvm::raw_fd_ostream(object_path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    return std::unexpected(
        aot_error{.message = std::format("failed to open `{}` for writing: {}",
                                         object_path, ec.message())});
  }

  auto pass_manager = llvm::legacy::PassManager{};
  if (target_machine->addPassesToEmitFile(pass_manager, dest, nullptr,
                                          llvm::CodeGenFileType::ObjectFile)) {
    return std::unexpected(aot_error{
        .message = "this target machine cannot emit a native object file"});
  }
  pass_manager.run(llvm_module);
  dest.flush();

  return {};
}

} // namespace kira::llvm_codegen
