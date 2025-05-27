#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/OptimizationLevel.h>

#include "ContractManager.hpp"
#include "ContractVerifierPreCall.hpp"
#include "ContractVerifierPostCall.hpp"
#include "ContractVerifierRelease.hpp"
#include "ContractPostProcess.hpp"

using namespace llvm;

bool FPMHook(StringRef Name, FunctionPassManager &FPM,
             ArrayRef<PassBuilder::PipelineElement>) {
    // Maybe future analyses can work on function level?
    return false;
};

bool MPMHook(StringRef Name, ModulePassManager &MPM,
             ArrayRef<PassBuilder::PipelineElement>) {
    if (Name == "contractVerifierPreCall") {
        MPM.addPass(ContractVerifierPreCallPass());
        return true;
    }
    if (Name == "contractVerifierPostCall") {
        MPM.addPass(ContractVerifierPostCallPass());
        return true;
    }
    if (Name == "contractVerifierRelease") {
        MPM.addPass(ContractVerifierReleasePass());
        return true;
    }
    if (Name == "contractPostProcess") {
        MPM.addPass(ContractPostProcessingPass());
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

llvm::PassPluginLibraryInfo getCoVerInfo() {
  return {LLVM_PLUGIN_API_VERSION, "CoVerPlugin",
          LLVM_VERSION_STRING, PBHook};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getCoVerInfo();
}
