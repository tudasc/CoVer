#pragma once

#include "llvm/IR/PassManager.h"
#include <map>
#include <set>

namespace llvm {

class ContractVerifierPreCallPass : public PassInfoMixin<ContractVerifierPreCallPass> {
    public:
        enum struct CallStatus { CALLED, NOTCALLED, ERROR };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    private:
        CallStatus checkPreCall(std::string reqFunc, const Function* F, const Module& M, std::string& error);
};

} // namespace llvm
