#pragma once

#include "ContractTree.hpp"
#include "ContractManager.hpp"
#include <set>

import LLVMModule;
import ErrorMessage;

namespace llvm {

class ContractVerifierPostCallPass : public PassInfoMixin<ContractVerifierPostCallPass> {
    public:
        enum struct CallStatus { CALLED, NOTCALLED };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

        static std::string postCallStatusToStr(ContractVerifierPostCallPass::CallStatus S) {
            switch (S) {
                case CallStatus::CALLED: return "CALLED";
                case CallStatus::NOTCALLED: return "NOTCALLED";
            }
        }

    private:
        CallStatus checkPostCall(const ContractTree::CallOperation* cOP, const ContractManagerAnalysis::LinearizedContract& C, ContractExpression& Expr, const bool isTag, const Module& M, std::string& error);
        ContractManagerAnalysis::ContractDatabase* DB;
        std::pair<CallStatus,bool> mergePostCallStat(CallStatus prev, CallStatus cur, const Instruction* I, void* data);
        CallStatus transferPostCallStat(CallStatus cur, const Instruction* I, void* data);
        ModuleAnalysisManager* MAM;

        static void appendDebugStr(std::string Target, bool isTag, const CallBase* Provider, const std::set<const CallBase *> candidates, std::vector<ErrorMessage>& err, const Instruction* retLoc);
};

} // namespace llvm
