#pragma once

#include "llvm/IR/PassManager.h"
#include "ContractTree.hpp"
#include "ContractManager.hpp"

namespace llvm {

class ContractVerifierReleasePass : public PassInfoMixin<ContractVerifierReleasePass> {
    public:
        enum struct ReleaseStatus { FULFILLED, FORBIDDEN, ERROR };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

        static std::string createDebugStr(const Instruction* Forbidden);
    private:
        ReleaseStatus checkRelease(const ContractTree::ReleaseOperation relOp, const ContractManagerAnalysis::LinearizedContract& C, const Module& M, std::string& error);
        std::map<const Function*, std::vector<TagUnit>> Tags;
};

} // namespace llvm
