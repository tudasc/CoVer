#pragma once

#include "llvm/IR/PassManager.h"
#include "ContractManager.hpp"

namespace llvm {

class ContractPostProcessingPass : public PassInfoMixin<ContractPostProcessingPass> {
    public:
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
    
    private:
        int xsucc, xfail, FP, FN, UN;
        void checkExpErr(ContractManagerAnalysis::Contract C);
};

} // namespace llvm
