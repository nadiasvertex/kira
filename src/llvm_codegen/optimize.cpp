#include "src/llvm_codegen/codegen.h"

#include <optional>
#include <string_view>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wshadow"
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#pragma clang diagnostic pop

namespace cinder::llvm_codegen {

auto parse_optimization_level(std::string_view text)
    -> std::optional<optimization_level> {
  if (text == "0") {
    return optimization_level::o0;
  }
  if (text == "1") {
    return optimization_level::o1;
  }
  if (text == "2") {
    return optimization_level::o2;
  }
  if (text == "3") {
    return optimization_level::o3;
  }
  return std::nullopt;
}

auto optimize_module(llvm::Module &module, optimization_level level) -> void {
  if (level == optimization_level::o0) {
    // `o0` stays a true no-op rather than running an "empty" pipeline, so a
    // `-O0` build's IR is byte-for-byte what the compiler produced.
    return;
  }
  auto llvm_level = llvm::OptimizationLevel::O1;
  switch (level) {
  case optimization_level::o2:
    llvm_level = llvm::OptimizationLevel::O2;
    break;
  case optimization_level::o3:
    llvm_level = llvm::OptimizationLevel::O3;
    break;
  default:
    break;
  }

  // Standard `llvm::PassBuilder` setup for the new pass manager — the same
  // analysis-manager wiring every out-of-tree LLVM frontend uses to run its
  // default per-module optimization pipeline (mirrors `clang -O2`'s own
  // pipeline construction, just without clang's extra frontend-specific
  // pass customization, which this teaching compiler has no need for).
  auto loop_analysis_manager = llvm::LoopAnalysisManager{};
  auto function_analysis_manager = llvm::FunctionAnalysisManager{};
  auto cgscc_analysis_manager = llvm::CGSCCAnalysisManager{};
  auto module_analysis_manager = llvm::ModuleAnalysisManager{};

  auto pass_builder = llvm::PassBuilder{};
  pass_builder.registerModuleAnalyses(module_analysis_manager);
  pass_builder.registerCGSCCAnalyses(cgscc_analysis_manager);
  pass_builder.registerFunctionAnalyses(function_analysis_manager);
  pass_builder.registerLoopAnalyses(loop_analysis_manager);
  pass_builder.crossRegisterProxies(
      loop_analysis_manager, function_analysis_manager, cgscc_analysis_manager,
      module_analysis_manager);

  auto module_pass_manager =
      pass_builder.buildPerModuleDefaultPipeline(llvm_level);
  module_pass_manager.run(module, module_analysis_manager);
}

} // namespace cinder::llvm_codegen
