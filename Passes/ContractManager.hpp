#pragma once

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/PassManager.h"

#include "../LangCode/ContractLangErrorListener.hpp"

namespace llvm {

class ContractManagerPass : public PassInfoMixin<ContractManagerPass> {
    public:
        // Run Pass
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    private:
        ContractLangErrorListener listener;
};

} // namespace llvm
