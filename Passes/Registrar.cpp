#include "ContractManager.hpp"
#include "ContractVerifierAlloc.hpp"
#include "ContractVerifierPreCall.hpp"
#include "ContractVerifierPostCall.hpp"
#include "ContractVerifierRelease.hpp"
#include "ContractVerifierParam.hpp"
#include "ContractPostProcess.hpp"
#include "Intrinsics.hpp"
#include "Instrument.hpp"

import LLVMModule;
import LLVMPassBuilder;
import AnalysisCleanup;
import BasicTypes;

using namespace llvm;

namespace {
    bool MPMHook(StringRef Name, ModulePassManager &MPM, ArrayRef<PassBuilder::PipelineElement>) {
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
        if (Name == "contractVerifierParam") {
            MPM.addPass(ContractVerifierParamPass());
            return true;
        }
        if (Name == "contractVerifierAlloc") {
            MPM.addPass(ContractVerifierAllocPass());
            return true;
        }
        if (Name == "contractPostProcess") {
            MPM.addPass(ContractPostProcessingPass());
            return true;
        }
        if (Name == "instrumentIntrinsics") {
            MPM.addPass(IntrinsicsPass());
            return true;
        }
        if (Name == "instrumentContracts") {
            MPM.addPass(InstrumentPass());
            return true;
        }
        if (Name == "analysisCleanup") {
            MPM.addPass(AnalysisCleanupPass());
            return true;
        }
        return false;
    };

    void MAMHook(ModuleAnalysisManager &MAM) {
        MAM.registerPass([&] { return BasicTypesAnalysis(); });
        MAM.registerPass([&] { return ContractManagerAnalysis(); });
    };

    void PBHook(PassBuilder &PB) {
        PB.registerPipelineParsingCallback(MPMHook);
        PB.registerAnalysisRegistrationCallback(MAMHook);
    }
}

llvm::PassPluginLibraryInfo getCoVerInfo() {
  return {CoVerPluginAPIVersion, "CoVerPlugin",
          CoVerLLVMVersionString, PBHook};
}

extern "C" __attribute__((weak)) ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getCoVerInfo();
}
