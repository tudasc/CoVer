#pragma once

#include "llvm/IR/PassManager.h"
#include <map>
#include <set>

namespace llvm {

class ContractVerifierPass : public PassInfoMixin<ContractVerifierPass> {
    public:
        enum struct RWStatus { READ, WRITE };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    private:
        std::set<RWStatus> checkVarRW(std::string var, const Function* F, bool must, std::string& error);

        std::map<const Instruction*, std::set<RWStatus>> WorklistRW(Value* val, const Instruction* Start, const Function* F, bool must);
};

} // namespace llvm
