#pragma once

#include "ContractTree.hpp"
#include "ContractManager.hpp"

import std;
import LLVMModule;
import Worklist;
import ErrorMessage;

namespace llvm {

class ContractVerifierReleasePass : public PassInfoMixin<ContractVerifierReleasePass> {
    public:
        enum struct ReleaseStatus { FULFILLED, FORBIDDEN, ERROR_UNFULFILLED, ERROR };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

        static void appendDebugStr(std::vector<ErrorMessage>& err, const Instruction* Forbidden, const CallBase* CB);
    private:
        bool printMultiReports = false;
        ReleaseStatus transferRelease(ReleaseStatus cur, const Instruction* I, void* data);
        std::pair<ReleaseStatus,bool> mergeRelease(ReleaseStatus prev, ReleaseStatus cur, const Instruction* I, void* data);
        ReleaseStatus checkRelease(ContractTree::ReleaseOperation const& relOp, ContractManagerAnalysis::LinearizedContract const& C, ContractExpression& Expr, const Module& M, std::string& error);
        ContractManagerAnalysis::ContractDatabase* DB;
        ModuleAnalysisManager* MAM;
};

} // namespace llvm
