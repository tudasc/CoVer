#pragma once

#include "ContractTree.hpp"
#include "ContractManager.hpp"
#include "ErrorMessage.h"
#include "llvm/IR/PassManager.h"
#include <llvm/IR/InstrTypes.h>
#include <map>
#include <set>
#include <vector>

namespace llvm {

class ContractVerifierPreCallPass : public PassInfoMixin<ContractVerifierPreCallPass> {
    public:
        enum struct CallStatusVal { CALLED, PARAMCHECK, NOTCALLED, ERROR };
        struct CallStatus {
            CallStatusVal CurVal;
            std::set<const CallBase*> candidate;
        };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

        static void appendDebugStr(std::string Target, bool isTag, const CallBase* Provider, const std::set<const CallBase *> candidates, std::vector<ErrorMessage>& err);

    private:
        std::pair<CallStatus,bool> mergePreCallStat(CallStatus prev, CallStatus cur, const Instruction* I, void* data);
        CallStatus transferPreCallStat(CallStatus cur, const Instruction* I, void* data);
        CallStatusVal checkPreCall(const ContractTree::CallOperation* cOP, const ContractManagerAnalysis::LinearizedContract& C, ContractExpression const& Expr, const bool isTag, const Module& M, std::string& error);
        std::map<const Function*, std::vector<TagUnit>> Tags;
};

} // namespace llvm
