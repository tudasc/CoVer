#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {

class ContractVerifierPass : public PassInfoMixin<ContractVerifierPass> {
    public:
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm
