#pragma once

#include "llvm/IR/PassManager.h"
#include <map>
#include <set>

namespace llvm {

class ContractVerifierRWPass : public PassInfoMixin<ContractVerifierRWPass> {
    public:
        enum struct RWStatus { READ, WRITE };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    private:
        std::set<RWStatus> checkVarRW(std::string var, const Function* F, bool must, std::string& error);
};

} // namespace llvm
