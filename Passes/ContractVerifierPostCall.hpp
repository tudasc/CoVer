#pragma once

#include "ContractTree.hpp"
#include "ContractManager.hpp"
#include "ErrorMessage.h"
#include "llvm/IR/PassManager.h"
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <map>
#include <set>

namespace llvm {

class ContractVerifierPostCallPass : public PassInfoMixin<ContractVerifierPostCallPass> {
    public:
        enum struct CallStatus { CALLED, NOTCALLED };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

        static void appendDebugStr(std::string Target, bool isTag, const CallBase* Provider, const std::set<const CallBase *> candidates, std::vector<ErrorMessage>& err, const Instruction* retLoc);

    private:
        CallStatus checkPostCall(const ContractTree::CallOperation* cOP, const ContractManagerAnalysis::LinearizedContract& C, ContractExpression const& Expr, const bool isTag, const Module& M, std::string& error);
        std::map<const Function*, std::vector<TagUnit>> Tags;
};

} // namespace llvm
