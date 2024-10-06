#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/OptimizationLevel.h>

#include "ContractManager.hpp"
#include "ContractVerifier.hpp"

using namespace llvm;

bool FPMHook(StringRef Name, FunctionPassManager &FPM,
             ArrayRef<PassBuilder::PipelineElement>) {
    // Maybe future analyses can work on function level?
    return false;
};

bool MPMHook(StringRef Name, ModulePassManager &MPM,
             ArrayRef<PassBuilder::PipelineElement>) {
    if (Name == "contractVerifier") {
        MPM.addPass(ContractVerifierPass());
        return true;
    }
    return false;
};

void MAMHook(ModuleAnalysisManager &MAM) {
    MAM.registerPass([&] { return ContractManagerAnalysis(); });
};

void PBHook(PassBuilder &PB) {
    PB.registerPipelineParsingCallback(FPMHook);
    PB.registerPipelineParsingCallback(MPMHook);
    PB.registerAnalysisRegistrationCallback(MAMHook);
}

llvm::PassPluginLibraryInfo getLLVMContractsPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "LLVMContractsPlugin",
          LLVM_VERSION_STRING, PBHook};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getLLVMContractsPluginInfo();
}
