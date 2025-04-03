#pragma once

#include "ContractTree.hpp"
#include "llvm/IR/PassManager.h"
#include <llvm/IR/InstrTypes.h>

namespace llvm {

class ContractVerifierParamPass : public PassInfoMixin<ContractVerifierParamPass> {
    public:
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    private:
        ModuleAnalysisManager* MAM;

        ContractTree::Fulfillment checkParamReq(const Value* var, const Value* contrVal, ContractTree::Comparator comp, bool isPtr, std::string& ErrInfo);
};

} // namespace llvm
