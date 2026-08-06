#pragma once

#include "ContractTree.hpp"
#include "ContractManager.hpp"
#include <set>
#include <vector>

import LLVMModule;
import ErrorMessage;

namespace llvm {

class ContractVerifierPreCallPass : public PassInfoMixin<ContractVerifierPreCallPass> {
    public:
        enum struct CallStatusVal { CALLED, PARAMCHECK, NOTCALLED, ERROR };
        struct CallStatus {
            CallStatusVal CurVal;
            std::set<const CallBase*> candidate;
        };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

        static std::string preCallStatusToStr(ContractVerifierPreCallPass::CallStatus S) {
            switch (S.CurVal) {
                case CallStatusVal::CALLED: return "CALLED";
                case CallStatusVal::NOTCALLED: return "NOTCALLED";
                case CallStatusVal::PARAMCHECK: return "PARAMCHECK";
                case CallStatusVal::ERROR: return "ERROR";
            }
        }
    private:
        std::pair<CallStatus,bool> mergePreCallStat(CallStatus prev, CallStatus cur, const Instruction* I, void* data);
        CallStatus transferPreCallStat(CallStatus cur, const Instruction* I, void* data);
        CallStatusVal checkPreCall(const ContractTree::CallOperation* cOP, const ContractManagerAnalysis::LinearizedContract& C, ContractExpression& Expr, const bool isTag, const Module& M, std::string& error);
        ContractManagerAnalysis::ContractDatabase* DB;
        ModuleAnalysisManager* MAM;

        static void appendDebugStr(std::string Target, bool isTag, const CallBase* Provider, const std::set<const CallBase *> candidates, std::vector<ErrorMessage>& err);
};

} // namespace llvm
