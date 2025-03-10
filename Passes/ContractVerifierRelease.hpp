#pragma once

#include "llvm/IR/PassManager.h"
#include <llvm/IR/InstrTypes.h>
#include "ContractTree.hpp"
#include "ContractManager.hpp"
#include "ErrorMessage.h"

namespace llvm {

class ContractVerifierReleasePass : public PassInfoMixin<ContractVerifierReleasePass> {
    public:
        enum struct ReleaseStatus { FULFILLED, FORBIDDEN, ERROR };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

        static void appendDebugStr(std::vector<ErrorMessage>& err, const Instruction* Forbidden, const CallBase* CB);
    private:
        ReleaseStatus checkRelease(const ContractTree::ReleaseOperation relOp, ContractManagerAnalysis::LinearizedContract const& C, ContractExpression const& Expr, const Module& M, std::string& error);
        std::map<const Function*, std::vector<TagUnit>> Tags;
};

} // namespace llvm
