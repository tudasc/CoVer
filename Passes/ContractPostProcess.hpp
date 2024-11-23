#pragma once

#include "llvm/IR/PassManager.h"
#include <memory>
#include "ContractManager.hpp"
#include "ContractTree.hpp"

namespace llvm {

class ContractPostProcessingPass : public PassInfoMixin<ContractPostProcessingPass> {
    public:
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    private:
        int xsucc, xfail, FP, FN, UN;
        void checkExpErr(ContractManagerAnalysis::Contract C);
        Fulfillment checkExpressions(ContractManagerAnalysis::Contract const& C, bool output);
        Fulfillment resolveFormula(std::shared_ptr<ContractFormula> contrF);
};

} // namespace llvm
