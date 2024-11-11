#pragma once

#include "llvm/IR/PassManager.h"
#include "ContractTree.hpp"
#include "ContractManager.hpp"

namespace llvm {

class ContractVerifierReleasePass : public PassInfoMixin<ContractVerifierReleasePass> {
    public:
        enum struct ReleaseStatus { FULFILLED, FORBIDDEN, ERROR };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    private:
        ReleaseStatus checkRelease(const ContractTree::ReleaseOperation relOp, const ContractManagerAnalysis::Contract& C, const Module& M, std::string& error);
        std::map<const Function*, std::vector<std::string>> Tags;
};

} // namespace llvm
