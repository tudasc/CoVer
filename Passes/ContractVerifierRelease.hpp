#pragma once

#include "llvm/IR/PassManager.h"
#include "ContractTree.hpp"
#include <map>
#include <set>

namespace llvm {

class ContractVerifierReleasePass : public PassInfoMixin<ContractVerifierReleasePass> {
    public:
        enum struct ReleaseStatus { FULFILLED, FORBIDDEN, ERROR };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    private:
        ReleaseStatus checkRelease(const ContractTree::ReleaseOperation relOp, const Function* F, const Module& M, std::string& error);
};

} // namespace llvm
